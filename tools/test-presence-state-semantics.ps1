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
$root = Join-Path $repo 'artifacts\presence-state-semantics'
$fixture = Join-Path $root 'untagged-presence-fixture.wav'
$rpcLog = Join-Path $root 'rpc.jsonl'
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $root 'manifest.json'
}
$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
[IO.Directory]::CreateDirectory($root) | Out-Null
foreach ($required in @($appFull, $probe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Presence state QA input is missing: $required"
    }
}

function Write-UntaggedFixture([string]$Path) {
    $sampleRate = 48000
    $frames = $sampleRate * 4
    $channels = 2
    $dataBytes = $frames * $channels * 2
    $writer = [IO.BinaryWriter]::new([IO.File]::Create($Path))
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
                [Math]::Sin(2.0 * [Math]::PI * 330.0 * $frame / $sampleRate) *
                900.0))
            $writer.Write($sample)
            $writer.Write($sample)
        }
    }
    finally { $writer.Dispose() }
}

function Invoke-Probe([string]$Action, [string]$Argument = '', [switch]$AllowFailure) {
    $arguments = @($appFull, $Action)
    if ($Argument) { $arguments += $Argument }
    $arguments += '--background'
    $output = & $probe @arguments 2>&1
    $exitCode = $LASTEXITCODE
    $text = ($output | ForEach-Object { $_.ToString() }) -join "`n"
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "Presence state probe '$Action' failed (${exitCode}): $text"
    }
    return [pscustomobject]@{ Success = $exitCode -eq 0; Output = $text }
}

function Read-Activities {
    $result = [Collections.Generic.List[object]]::new()
    # The fake Discord pipe appends while the UI is being sampled. Read a
    # point-in-time snapshot with sharing enabled so a legitimate concurrent
    # append cannot turn the semantic assertion into a file-lock flake.
    $stream = [IO.FileStream]::new(
        $rpcLog,
        [IO.FileMode]::OpenOrCreate,
        [IO.FileAccess]::Read,
        [IO.FileShare]::ReadWrite)
    try {
        $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8,
            $true, 4096, $false)
        try { $lines = @($reader.ReadToEnd() -split "`r?`n") }
        finally { $reader.Dispose() }
    }
    finally { $stream.Dispose() }
    foreach ($line in $lines) {
        if (-not $line) { continue }
        try {
            $entry = $line | ConvertFrom-Json
            $payload = $entry.json | ConvertFrom-Json
            $result.Add([pscustomobject]@{
                Ticks = [int64]$entry.ticks
                Activity = $payload.args.activity
            })
        } catch {}
    }
    return @($result)
}

function Wait-Activity([int]$AfterCount, [scriptblock]$Predicate,
        [int]$TimeoutMs = 4500) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    do {
        $activities = @(Read-Activities)
        for ($index = $AfterCount; $index -lt $activities.Count; ++$index) {
            if (& $Predicate $activities[$index].Activity) {
                return [pscustomobject]@{
                    Entry = $activities[$index]
                    Count = $activities.Count
                    ElapsedMilliseconds = $timer.Elapsed.TotalMilliseconds
                }
            }
        }
        Start-Sleep -Milliseconds 25
    } while ($timer.ElapsedMilliseconds -lt $TimeoutMs)
    throw "Expected Discord activity was not published within ${TimeoutMs}ms."
}

function Stop-RunningCandidate {
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object { $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $appFull, [StringComparison]::OrdinalIgnoreCase) })
    if ($matches.Count -gt 1) { throw 'Multiple Presence candidates are running.' }
    if ($matches.Count -eq 0) { return }
    & $appFull /exit | Out-Null
    for ($attempt = 0; $attempt -lt 200; ++$attempt) {
        if (-not (Get-Process -Id $matches[0].ProcessId -ErrorAction SilentlyContinue)) {
            return
        }
        Start-Sleep -Milliseconds 25
    }
    throw 'Existing Presence candidate did not exit cleanly.'
}

Write-UntaggedFixture $fixture
[IO.File]::WriteAllText($rpcLog, '')
$pipeName = 'goden-presence-state-' + [guid]::NewGuid().ToString('N')
$rpcJob = Start-Job -ArgumentList $pipeName, $rpcLog -ScriptBlock {
    param($Name, $Log)
    function Read-Exact([IO.Stream]$Stream, [int]$Count) {
        $bytes = [byte[]]::new($Count)
        $offset = 0
        while ($offset -lt $Count) {
            $read = $Stream.Read($bytes, $offset, $Count - $offset)
            if ($read -le 0) { return $null }
            $offset += $read
        }
        return $bytes
    }
    function Read-Frame([IO.Stream]$Stream) {
        $header = Read-Exact $Stream 8
        if ($null -eq $header) { return $null }
        $length = [BitConverter]::ToUInt32($header, 4)
        if ($length -gt 1048576) { return $null }
        $body = Read-Exact $Stream ([int]$length)
        if ($null -eq $body) { return $null }
        return [pscustomobject]@{
            Opcode = [BitConverter]::ToUInt32($header, 0)
            Json = [Text.Encoding]::UTF8.GetString($body)
        }
    }
    function Write-Frame([IO.Stream]$Stream, [uint32]$Opcode, [string]$Json) {
        $body = [Text.Encoding]::UTF8.GetBytes($Json)
        $header = [byte[]]::new(8)
        [BitConverter]::GetBytes($Opcode).CopyTo($header, 0)
        [BitConverter]::GetBytes([uint32]$body.Length).CopyTo($header, 4)
        $Stream.Write($header, 0, $header.Length)
        $Stream.Write($body, 0, $body.Length)
        $Stream.Flush()
    }
    $pipe = [IO.Pipes.NamedPipeServerStream]::new(
        $Name, [IO.Pipes.PipeDirection]::InOut, 1,
        [IO.Pipes.PipeTransmissionMode]::Byte,
        [IO.Pipes.PipeOptions]::Asynchronous)
    try {
        $pipe.WaitForConnection()
        $handshake = Read-Frame $pipe
        if ($null -eq $handshake) { return }
        Write-Frame $pipe 1 '{"cmd":"DISPATCH","evt":"READY","data":{}}'
        while ($pipe.IsConnected) {
            $frame = Read-Frame $pipe
            if ($null -eq $frame) { break }
            if ($frame.Opcode -ne 1) { continue }
            [pscustomobject]@{
                ticks = [Diagnostics.Stopwatch]::GetTimestamp()
                json = $frame.Json
            } | ConvertTo-Json -Compress |
                Add-Content -LiteralPath $Log -Encoding UTF8
            $request = $frame.Json | ConvertFrom-Json
            $ack = @{
                cmd = 'SET_ACTIVITY'
                data = $null
                evt = $null
                nonce = $request.nonce
            } | ConvertTo-Json -Compress
            Write-Frame $pipe 1 $ack
        }
    }
    finally { $pipe.Dispose() }
}

$process = $null
try {
    Stop-RunningCandidate
    $env:NEKOROX_QA_HOOKS = '1'
    $env:NEKOROX_QA_PROFILE = '1'
    $env:NEKOROX_QA_BACKGROUND = '1'
    $env:NEKOROX_QA_DISCORD_PIPE = $pipeName
    $process = Start-Process -FilePath $appFull -WindowStyle Hidden -PassThru
    Remove-Item Env:NEKOROX_QA_HOOKS,Env:NEKOROX_QA_PROFILE,
        Env:NEKOROX_QA_BACKGROUND,Env:NEKOROX_QA_DISCORD_PIPE `
        -ErrorAction SilentlyContinue
    for ($attempt = 0; $attempt -lt 200; ++$attempt) {
        $window = Invoke-Probe 'window-state' -AllowFailure
        if ($window.Success) { break }
        Start-Sleep -Milliseconds 25
    }
    if (-not $window.Success) { throw 'Presence candidate did not start.' }
    Start-Sleep -Milliseconds 750
    Invoke-Probe 'options-open' | Out-Null
    Invoke-Probe 'covers-disable' | Out-Null
    Invoke-Probe 'paused-hide' | Out-Null
    Invoke-Probe 'presence-enable' | Out-Null
    Invoke-Probe 'drop-file' $fixture | Out-Null

    $playing = Wait-Activity 0 {
        param($activity)
        $null -ne $activity -and
            $null -ne $activity.PSObject.Properties['timestamps'] -and
            $null -ne $activity.timestamps.PSObject.Properties['end'] -and
            $activity.details -eq 'untagged-presence-fixture'
    }
    if ($playing.Entry.Activity.state -ne
            'Local file · untagged-presence-fixture.wav') {
        throw "Untagged local-file Presence is wrong: $($playing.Entry.Activity.state)"
    }
    $count = $playing.Count
    Invoke-Probe 'play' | Out-Null
    $paused = Wait-Activity $count {
        param($activity)
        $null -eq $activity
    } 600

    $count = $paused.Count
    Invoke-Probe 'play' | Out-Null
    $resumed = Wait-Activity $count {
        param($activity)
        $null -ne $activity -and
            $null -ne $activity.PSObject.Properties['timestamps'] -and
            $null -ne $activity.timestamps.PSObject.Properties['end'] -and
            $activity.details -eq 'untagged-presence-fixture'
    } 600

    Invoke-Probe 'daw-open' | Out-Null
    Invoke-Probe 'daw-drop' $fixture | Out-Null
    for ($attempt = 0; $attempt -lt 240; ++$attempt) {
        $state = Invoke-Probe 'daw-state' -AllowFailure
        if ($state.Success -and $state.Output -match 'audio=1') { break }
        Start-Sleep -Milliseconds 25
    }
    if (-not $state.Success -or $state.Output -notmatch 'audio=1') {
        throw 'DAW fixture did not decode for Presence QA.'
    }
    $count = @(Read-Activities).Count
    Invoke-Probe 'daw-play-toggle' | Out-Null
    $dawPlaying = Wait-Activity $count {
        param($activity)
        $null -ne $activity -and $activity.details -like 'DAW Mode*' -and
            $activity.state -like 'Previewing*' -and
            $null -ne $activity.PSObject.Properties['timestamps']
    } 800
    $count = $dawPlaying.Count
    Invoke-Probe 'daw-play-toggle' | Out-Null
    $dawPaused = Wait-Activity $count {
        param($activity)
        $null -eq $activity
    } 800

    $manifest = [ordered]@{
        schema = 'goden.presence-state-semantics.v1'
        createdUtc = [DateTime]::UtcNow.ToString('o')
        app = $appFull
        fixture = $fixture
        passed = $true
        localFileState = $playing.Entry.Activity.state
        pauseUpdateMilliseconds = $paused.ElapsedMilliseconds
        resumeUpdateMilliseconds = $resumed.ElapsedMilliseconds
        dawPlayingDetails = $dawPlaying.Entry.Activity.details
        pausedHidden = $true
        dawPauseUpdateMilliseconds = $dawPaused.ElapsedMilliseconds
    }
    $manifest | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath $manifestFull -Encoding UTF8
    Write-Host 'Goden Discord Presence state semantics: PASS'
    Write-Host "Evidence: $manifestFull"
}
finally {
    Remove-Item Env:NEKOROX_QA_HOOKS,Env:NEKOROX_QA_PROFILE,
        Env:NEKOROX_QA_BACKGROUND,Env:NEKOROX_QA_DISCORD_PIPE `
        -ErrorAction SilentlyContinue
    if ($null -ne $process -and -not $process.HasExited) {
        & $appFull /exit | Out-Null
        if (-not $process.WaitForExit(5000)) {
            Stop-Process -Id $process.Id -Force
        }
    }
    Remove-Job $rpcJob -Force -ErrorAction SilentlyContinue
}
