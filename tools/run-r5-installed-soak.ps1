param(
    [Parameter(Mandatory = $true)]
    [string]$DevRoot,
    [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$MediaRoot,
    [ValidateRange(0.1, 240.0)]
    [double]$DurationMinutes = 30.0,
    [string]$ManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$artifactRoot = [IO.Path]::GetFullPath(
    (Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
$devRootFull = [IO.Path]::GetFullPath($DevRoot).TrimEnd('\')
if (-not ($devRootFull + '\').StartsWith(
        $artifactRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "R5 soak is restricted to disposable artifacts: $devRootFull"
}
$app = Join-Path $devRootFull 'app\foobar2000.exe'
$component = Join-Path $devRootFull `
    'app\profile\user-components-x64\foo_nekorox\foo_nekorox.dll'
$probe = Join-Path $repo 'out\Release\nekorox_ui_probe.exe'
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\r5\soak30-final.json'
}
$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
foreach ($required in @($app, $component, $probe, $MediaRoot)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "R5 soak input is missing: $required"
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
        throw "R5 soak probe '$Action' failed (${exitCode}): $text"
    }
    return [pscustomobject]@{ Success = $exitCode -eq 0; Output = $text }
}

function Stop-Candidate([Diagnostics.Process]$Process) {
    if ($null -eq $Process) { return }
    $Process.Refresh()
    if ($Process.HasExited) { return }
    Invoke-Probe 'app-close' -Foreground -AllowFailure | Out-Null
    if (-not $Process.WaitForExit(10000)) {
        throw 'R5 soak candidate did not close gracefully.'
    }
}

$files = @(rg --files $MediaRoot |
    Where-Object { $_ -match '\.(flac|mp3|m4a|aac|ogg|opus|wav)$' } |
    Group-Object { Split-Path -Parent $_ } |
    Where-Object Count -ge 3 |
    Select-Object -First 10 |
    ForEach-Object { $_.Group | Select-Object -First 3 })
if ($files.Count -lt 30) {
    throw 'R5 soak needs ten authorized album folders with three tracks each.'
}

Invoke-Probe 'app-close' -Foreground -AllowFailure | Out-Null
Start-Sleep -Milliseconds 300
$env:NEKOROX_QA_HOOKS = '1'
$env:NEKOROX_QA_PROFILE = '1'
$env:NEKOROX_QA_BACKGROUND = '1'
$process = $null
try {
    $process = Start-Process -FilePath $app `
        -WorkingDirectory (Split-Path -Parent $app) `
        -WindowStyle Hidden -PassThru
}
finally {
    Remove-Item Env:NEKOROX_QA_HOOKS, Env:NEKOROX_QA_PROFILE,
        Env:NEKOROX_QA_BACKGROUND -ErrorAction SilentlyContinue
}

$samples = [Collections.Generic.List[object]]::new()
$actions = [Collections.Generic.List[object]]::new()
$watch = [Diagnostics.Stopwatch]::StartNew()
$startedUtc = [DateTime]::UtcNow
try {
    $ready = $false
    for ($attempt = 0; $attempt -lt 200; ++$attempt) {
        if ((Invoke-Probe 'window-state' -AllowFailure).Success) {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 25
    }
    if (-not $ready) { throw 'R5 soak candidate did not publish its window.' }
    Invoke-Probe 'window-offscreen-show' | Out-Null
    Invoke-Probe 'mute-on' | Out-Null
    $process.Refresh()
    $handleStart = $process.HandleCount
    $threadStart = $process.Threads.Count
    $workingSetStart = $process.WorkingSet64
    $cpuStart = $process.TotalProcessorTime.TotalMilliseconds
    $trackIndex = 0
    $listenCycles = 0
    $nextTrackAt = 0.0
    $nextListenAt = 0.0
    $nextSampleAt = 0.0
    while ($watch.Elapsed.TotalMinutes -lt $DurationMinutes) {
        $seconds = $watch.Elapsed.TotalSeconds
        if ($trackIndex -lt $files.Count -and $seconds -ge $nextTrackAt) {
            Invoke-Probe 'drop-file' $files[$trackIndex] | Out-Null
            Start-Sleep -Milliseconds 250
            Invoke-Probe 'shell-album' | Out-Null
            $actions.Add([pscustomobject]@{
                kind = 'track'
                index = $trackIndex
                second = [Math]::Round($seconds, 1)
                album = Split-Path -Leaf (Split-Path -Parent $files[$trackIndex])
                fileSha256 = (Get-FileHash -LiteralPath $files[$trackIndex] `
                    -Algorithm SHA256).Hash
            })
            ++$trackIndex
            $nextTrackAt += 18.0
        }
        if ($seconds -ge $nextListenAt) {
            # Recreate rather than merely hide/show: this exercises the CRT
            # callback admission/drain boundary that previously froze the host
            # after leaving Listen. The candidate itself is QA-background and
            # its main window remains off-screen.
            Invoke-Probe 'listen' -Foreground | Out-Null
            Start-Sleep -Milliseconds 80
            $focus = @('listen-focus-scope', 'listen-focus-crt',
                'listen-focus-spectrum', 'listen-focus-spectrogram')[
                    $listenCycles % 4]
            Invoke-Probe $focus | Out-Null
            Start-Sleep -Milliseconds 100
            Invoke-Probe 'listen-close' | Out-Null
            Invoke-Probe 'shell-library' | Out-Null
            ++$listenCycles
            $nextListenAt += 60.0
        }
        if ($seconds -ge $nextSampleAt) {
            $process.Refresh()
            if ($process.HasExited -or -not $process.Responding) {
                throw "R5 soak host became unhealthy at ${seconds}s."
            }
            $samples.Add([pscustomobject]@{
                minute = [Math]::Round($watch.Elapsed.TotalMinutes, 2)
                cpuMilliseconds =
                    $process.TotalProcessorTime.TotalMilliseconds - $cpuStart
                handles = $process.HandleCount
                threads = $process.Threads.Count
                workingSet = $process.WorkingSet64
                responding = $process.Responding
            })
            $nextSampleAt += 60.0
        }
        Start-Sleep -Milliseconds 200
    }
    $process.Refresh()
    $result = [ordered]@{
        schema = 'goden.r5.soak30.v1'
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        startedUtc = $startedUtc.ToString('o')
        componentSha256 = (Get-FileHash -LiteralPath $component `
            -Algorithm SHA256).Hash
        status = 'pass'
        durationMinutes = $watch.Elapsed.TotalMinutes
        authorizedAlbums = 10
        tracksExercised = $trackIndex
        listenCycles = $listenCycles
        handleStart = $handleStart
        handleEnd = $process.HandleCount
        threadStart = $threadStart
        threadEnd = $process.Threads.Count
        workingSetStart = $workingSetStart
        workingSetEnd = $process.WorkingSet64
        samples = $samples
        actions = $actions
    }
    New-Item -ItemType Directory -Force `
        -Path (Split-Path -Parent $manifestFull) | Out-Null
    $result | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $manifestFull -Encoding UTF8
    $result | ConvertTo-Json -Depth 4
}
finally {
    Stop-Candidate $process
}
