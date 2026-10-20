param(
    [Parameter(Mandatory = $true)]
    [string]$App,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$ManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$appFull = [IO.Path]::GetFullPath($App)
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\daw-async-lifecycle.json'
}
$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
$fixture = Join-Path $repo 'artifacts\daw-async-fixture.wav'
foreach ($required in @($appFull, $probe)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "DAW lifecycle QA input is missing: $required"
    }
}
[IO.Directory]::CreateDirectory((Split-Path -Parent $fixture)) | Out-Null
[IO.Directory]::CreateDirectory((Split-Path -Parent $manifestFull)) | Out-Null

function Write-Fixture([string]$Path) {
    $sampleRate = 48000
    $frames = [int]($sampleRate * 0.45)
    $channels = 2
    $dataBytes = $frames * $channels * 2
    $stream = [IO.File]::Create($Path)
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
        $writer.Write([int](36 + $dataBytes))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVE'))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('fmt '))
        $writer.Write([int]16)
        $writer.Write([int16]1)
        $writer.Write([int16]$channels)
        $writer.Write([int]$sampleRate)
        $writer.Write([int]($sampleRate * $channels * 2))
        $writer.Write([int16]($channels * 2))
        $writer.Write([int16]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data'))
        $writer.Write([int]$dataBytes)
        for ($frame = 0; $frame -lt $frames; ++$frame) {
            $sample = [int16]([Math]::Round(
                [Math]::Sin(2.0 * [Math]::PI * 220.0 * $frame / $sampleRate) *
                1200.0))
            $writer.Write($sample)
            $writer.Write($sample)
        }
    }
    finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

function Invoke-Probe(
    [string]$Action,
    [string]$Argument = '',
    [switch]$AllowFailure
) {
    $arguments = @($appFull, $Action)
    if ($Argument) { $arguments += $Argument }
    $arguments += '--background'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $output = & $probe @arguments 2>&1
    $exitCode = $LASTEXITCODE
    $watch.Stop()
    $text = ($output | ForEach-Object { $_.ToString() }) -join "`n"
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "UI probe '$Action' failed (${exitCode}): $text"
    }
    return [pscustomobject]@{
        Success = $exitCode -eq 0
        Output = $text
        ElapsedMilliseconds = $watch.Elapsed.TotalMilliseconds
    }
}

function Wait-Lifecycle([string]$Expected, [int]$TimeoutMs = 5000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Invoke-Probe 'daw-lifecycle' -AllowFailure
        if ($state.Success -and $state.Output -match
                'state=(\w+)\s+result=(\d+)\s+generation=(\d+)') {
            if ($Matches[1] -eq $Expected) {
                return [pscustomobject]@{
                    State = $Matches[1]
                    Result = [uint32]$Matches[2]
                    Generation = [uint64]$Matches[3]
                    ElapsedMilliseconds = $watch.Elapsed.TotalMilliseconds
                }
            }
        }
        Start-Sleep -Milliseconds 25
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw "DAW did not enter '$Expected' within ${TimeoutMs}ms. Last state: $($state.Output)"
}

function Wait-AudioReady([int]$TimeoutMs = 10000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Invoke-Probe 'daw-state' -AllowFailure
        if ($state.Success -and $state.Output -match
                'tracks=(\d+)\s+ready=(\d+)\s+playing=(\d+)\s+audio=(\d+)\s+failed=(\d+)') {
            if ([int]$Matches[1] -eq 1 -and [int]$Matches[4] -eq 1) {
                return $watch.Elapsed.TotalMilliseconds
            }
            if ([int]$Matches[5] -ne 0) {
                throw "DAW fixture decode failed: $($state.Output)"
            }
        }
        Start-Sleep -Milliseconds 25
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw "DAW fixture was not decoded within ${TimeoutMs}ms. Last state: $($state.Output)"
}

function Stop-Candidate([Diagnostics.Process]$Process) {
    if ($null -eq $Process) { return }
    $Process.Refresh()
    if ($Process.HasExited) { return }
    & $appFull /exit | Out-Null
    if (-not $Process.WaitForExit(10000)) {
        Stop-Process -Id $Process.Id -Force
        throw 'Disposable DAW candidate required forced termination.'
    }
}

function Stop-RunningCandidate {
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and [IO.Path]::GetFullPath(
                $_.ExecutablePath).Equals(
                    $appFull, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($matches.Count -gt 1) {
        throw 'Multiple disposable DAW candidates are running.'
    }
    if ($matches.Count -eq 0) { return }
    & $appFull /exit | Out-Null
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        Start-Sleep -Milliseconds 50
        $alive = @(Get-Process -Id $matches[0].ProcessId -ErrorAction SilentlyContinue)
    } while ($alive.Count -ne 0 -and $watch.ElapsedMilliseconds -lt 10000)
    if ($alive.Count -ne 0) {
        throw 'Existing disposable DAW candidate did not exit cleanly.'
    }
}

Write-Fixture $fixture
$results = [Collections.Generic.List[object]]::new()
$faults = @('open', 'prepare', 'write', 'reset', 'unprepare', 'close')
foreach ($fault in $faults) {
    $process = $null
    try {
        Stop-RunningCandidate
        $env:NEKOROX_QA_HOOKS = '1'
        $env:NEKOROX_QA_PROFILE = '1'
        $env:NEKOROX_QA_BACKGROUND = '1'
        $env:NEKOROX_QA_DAW_FAULT = $fault
        $process = Start-Process -FilePath $appFull -WindowStyle Hidden -PassThru
        foreach ($name in @('NEKOROX_QA_HOOKS','NEKOROX_QA_PROFILE',
                'NEKOROX_QA_BACKGROUND','NEKOROX_QA_DAW_FAULT')) {
            Remove-Item "Env:$name" -ErrorAction SilentlyContinue
        }
        for ($attempt = 0; $attempt -lt 200; ++$attempt) {
            $window = Invoke-Probe 'window-state' -AllowFailure
            if ($window.Success) { break }
            $process.Refresh()
            if ($process.HasExited) { throw "Candidate exited during '$fault' startup." }
            Start-Sleep -Milliseconds 25
        }
        if (-not $window.Success) { throw "Candidate did not start for '$fault'." }
        Invoke-Probe 'mute-on' | Out-Null
        $open = Invoke-Probe 'daw-open'
        $drop = Invoke-Probe 'daw-drop' $fixture
        $decodeMs = Wait-AudioReady
        $play = Invoke-Probe 'daw-play-toggle'
        if ($play.ElapsedMilliseconds -gt 500.0) {
            throw "DAW play command blocked for $($play.ElapsedMilliseconds)ms ($fault)."
        }
        $error = Wait-Lifecycle 'error' 7000
        if ($error.Result -eq 0) {
            throw "Injected '$fault' reached error state without MMRESULT."
        }
        $close = Invoke-Probe 'daw-close'
        if ($close.ElapsedMilliseconds -gt 500.0) {
            throw "DAW close command blocked for $($close.ElapsedMilliseconds)ms ($fault)."
        }
        $results.Add([ordered]@{
            fault = $fault
            decodeMilliseconds = $decodeMs
            openCommandMilliseconds = $open.ElapsedMilliseconds
            dropCommandMilliseconds = $drop.ElapsedMilliseconds
            playCommandMilliseconds = $play.ElapsedMilliseconds
            errorObservedMilliseconds = $error.ElapsedMilliseconds
            closeCommandMilliseconds = $close.ElapsedMilliseconds
            result = $error.Result
            generation = $error.Generation
            passed = $true
        })
    }
    finally {
        foreach ($name in @('NEKOROX_QA_HOOKS','NEKOROX_QA_PROFILE',
                'NEKOROX_QA_BACKGROUND','NEKOROX_QA_DAW_FAULT')) {
            Remove-Item "Env:$name" -ErrorAction SilentlyContinue
        }
        Stop-Candidate $process
    }
}

$manifest = [ordered]@{
    schema = 'goden.daw-async-lifecycle.v2'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    app = $appFull
    fixture = $fixture
    passed = $true
    faults = @($results)
}
$manifest | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath $manifestFull -Encoding UTF8
Write-Host 'Goden DAW asynchronous lifecycle: PASS'
Write-Host "Evidence: $manifestFull"
