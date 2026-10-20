param(
    [Parameter(Mandatory = $true)]
    [string]$App,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateRange(1, 1000)]
    [int]$Cycles = 100,
    [string]$Fixture = '',
    [string]$ManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$appFull = [IO.Path]::GetFullPath($App)
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
if (-not $Fixture) {
    $Fixture = Join-Path $repo 'artifacts\daw-async-fixture.wav'
}
$fixtureFull = [IO.Path]::GetFullPath($Fixture)
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\daw-cycle-soak.json'
}
$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
foreach ($required in @($appFull, $probe, $fixtureFull)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "DAW cycle soak input is missing: $required"
    }
}
[IO.Directory]::CreateDirectory((Split-Path -Parent $manifestFull)) | Out-Null

function Get-CandidateProcesses {
    return @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and [IO.Path]::GetFullPath(
                $_.ExecutablePath).Equals(
                    $appFull, [StringComparison]::OrdinalIgnoreCase)
        })
}

function Stop-RunningCandidate {
    $matches = @(Get-CandidateProcesses)
    if ($matches.Count -gt 1) { throw 'Multiple DAW soak candidates are running.' }
    if ($matches.Count -eq 0) { return }
    & $appFull /exit | Out-Null
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        Start-Sleep -Milliseconds 50
        $alive = @(Get-Process -Id $matches[0].ProcessId -ErrorAction SilentlyContinue)
    } while ($alive.Count -ne 0 -and $watch.ElapsedMilliseconds -lt 10000)
    if ($alive.Count -ne 0) { throw 'DAW soak candidate did not exit cleanly.' }
}

function Invoke-Probe([string]$Action, [string]$Argument = '') {
    $arguments = @($appFull, $Action)
    if ($Argument) { $arguments += $Argument }
    $arguments += '--background'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $output = & $probe @arguments 2>&1
    $watch.Stop()
    if ($LASTEXITCODE -ne 0) {
        throw "DAW soak probe '$Action' failed: $($output -join "`n")"
    }
    return [pscustomobject]@{
        Output = ($output | ForEach-Object ToString) -join "`n"
        Milliseconds = $watch.Elapsed.TotalMilliseconds
    }
}

function Wait-Lifecycle([string]$Expected, [int]$TimeoutMs = 2500) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Invoke-Probe 'daw-lifecycle'
        if ($state.Output -match 'state=(\w+)\s+result=(\d+)\s+generation=(\d+)' -and
                $Matches[1] -eq $Expected) {
            return $watch.Elapsed.TotalMilliseconds
        }
        Start-Sleep -Milliseconds 10
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw "DAW did not enter '$Expected' within ${TimeoutMs}ms. Last: $($state.Output)"
}

function Wait-AudioReady([int]$TimeoutMs = 10000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Invoke-Probe 'daw-state'
        if ($state.Output -match 'tracks=(\d+)\s+ready=(\d+)\s+playing=(\d+)\s+audio=(\d+)\s+failed=(\d+)') {
            if ([int]$Matches[1] -eq 1 -and [int]$Matches[4] -eq 1) {
                return $watch.Elapsed.TotalMilliseconds
            }
            if ([int]$Matches[5] -ne 0) {
                throw "DAW fixture decode failed: $($state.Output)"
            }
        }
        Start-Sleep -Milliseconds 20
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw "DAW fixture was not ready within ${TimeoutMs}ms."
}

Stop-RunningCandidate
$process = $null
$commandSamples = [Collections.Generic.List[double]]::new()
$transitionSamples = [Collections.Generic.List[double]]::new()
try {
    $env:NEKOROX_QA_HOOKS = '1'
    $env:NEKOROX_QA_PROFILE = '1'
    $env:NEKOROX_QA_BACKGROUND = '1'
    $process = Start-Process -FilePath $appFull -WindowStyle Hidden -PassThru
    Remove-Item Env:NEKOROX_QA_HOOKS,Env:NEKOROX_QA_PROFILE,
        Env:NEKOROX_QA_BACKGROUND -ErrorAction SilentlyContinue
    for ($attempt = 0; $attempt -lt 200; ++$attempt) {
        try { Invoke-Probe 'window-state' | Out-Null; break } catch {}
        $process.Refresh()
        if ($process.HasExited) { throw 'DAW soak candidate exited during startup.' }
        Start-Sleep -Milliseconds 25
    }
    Invoke-Probe 'mute-on' | Out-Null
    Invoke-Probe 'daw-open' | Out-Null
    Invoke-Probe 'daw-drop' $fixtureFull | Out-Null
    $decodeMs = Wait-AudioReady

    for ($cycle = 0; $cycle -lt $Cycles; ++$cycle) {
        foreach ($step in @(
            [pscustomobject]@{ Action = 'daw-play-toggle'; State = 'playing' },
            [pscustomobject]@{ Action = 'daw-play-toggle'; State = 'paused' },
            [pscustomobject]@{ Action = 'daw-play-toggle'; State = 'playing' },
            [pscustomobject]@{ Action = 'daw-stop'; State = 'idle' }
        )) {
            $command = Invoke-Probe $step.Action
            $commandSamples.Add($command.Milliseconds)
            if ($command.Milliseconds -gt 500.0) {
                throw "DAW command exceeded 500ms in cycle ${cycle}: $($command.Milliseconds)ms"
            }
            $transitionSamples.Add((Wait-Lifecycle $step.State))
        }
    }
    Invoke-Probe 'daw-close' | Out-Null
    Start-Sleep -Milliseconds 250

    $manifest = [ordered]@{
        schema = 'goden.daw-cycle-soak.v1'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        passed = $true
        app = $appFull
        fixture = $fixtureFull
        cycles = $Cycles
        commands = $commandSamples.Count
        decodeMilliseconds = $decodeMs
        commandMaximumMilliseconds = ($commandSamples | Measure-Object -Maximum).Maximum
        transitionMaximumMilliseconds = ($transitionSamples | Measure-Object -Maximum).Maximum
    }
    $manifest | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath $manifestFull -Encoding UTF8
    Write-Host "Goden DAW cycle soak: PASS ($Cycles cycles)"
    Write-Host "Evidence: $manifestFull"
}
finally {
    Remove-Item Env:NEKOROX_QA_HOOKS,Env:NEKOROX_QA_PROFILE,
        Env:NEKOROX_QA_BACKGROUND -ErrorAction SilentlyContinue
    if ($null -ne $process -and -not $process.HasExited) {
        & $appFull /exit | Out-Null
        if (-not $process.WaitForExit(10000)) {
            throw 'DAW soak candidate did not exit cleanly after QA.'
        }
    }
}
