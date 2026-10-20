param(
    [Parameter(Mandatory = $true)]
    [string]$App,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$ManifestPath = '',
    [string]$FixtureTrack = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$appFull = [IO.Path]::GetFullPath($App)
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\presence-fault-matrix.json'
}
$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
$root = Join-Path $repo 'artifacts\presence-fault-matrix'
[IO.Directory]::CreateDirectory($root) | Out-Null
foreach ($required in @($appFull, $probe)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Presence fault QA input is missing: $required"
    }
}
if ($FixtureTrack) {
    $FixtureTrack = [IO.Path]::GetFullPath($FixtureTrack)
    if (-not (Test-Path -LiteralPath $FixtureTrack -PathType Leaf)) {
        throw "Presence track fixture is missing: $FixtureTrack"
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
    $output = & $probe @arguments 2>&1
    $exitCode = $LASTEXITCODE
    $text = ($output | ForEach-Object { $_.ToString() }) -join "`n"
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "Presence probe '$Action' failed (${exitCode}): $text"
    }
    return [pscustomobject]@{ Success = $exitCode -eq 0; Output = $text }
}

function New-Port {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try { return ([Net.IPEndPoint]$listener.LocalEndpoint).Port }
    finally { $listener.Stop() }
}

function Start-RpcServer([string]$Name, [string]$Mode, [string]$Log) {
    Start-Job -ArgumentList $Name, $Mode, $Log -ScriptBlock {
        param($Name, $Mode, $Log)
        function Read-Exact([IO.Stream]$Stream, [int]$Count) {
            $buffer = [byte[]]::new($Count)
            $offset = 0
            while ($offset -lt $Count) {
                $read = $Stream.Read($buffer, $offset, $Count - $offset)
                if ($read -le 0) { return $null }
                $offset += $read
            }
            return $buffer
        }
        function Read-Frame([IO.Stream]$Stream) {
            $header = Read-Exact $Stream 8
            if ($null -eq $header) { return $null }
            $opcode = [BitConverter]::ToUInt32($header, 0)
            $length = [BitConverter]::ToUInt32($header, 4)
            if ($length -gt 1048576) { return $null }
            $payload = Read-Exact $Stream ([int]$length)
            if ($null -eq $payload) { return $null }
            return [pscustomobject]@{
                Opcode = $opcode
                Json = [Text.Encoding]::UTF8.GetString($payload)
            }
        }
        function Write-Frame([IO.Stream]$Stream, [uint32]$Opcode, [string]$Json) {
            $payload = [Text.Encoding]::UTF8.GetBytes($Json)
            $header = [byte[]]::new(8)
            [BitConverter]::GetBytes($Opcode).CopyTo($header, 0)
            [BitConverter]::GetBytes([uint32]$payload.Length).CopyTo($header, 4)
            $Stream.Write($header, 0, 8)
            $Stream.Write($payload, 0, $payload.Length)
            $Stream.Flush()
        }
        $connections = if ($Mode -in @('close','malformed')) { 2 } else { 1 }
        for ($index = 0; $index -lt $connections; ++$index) {
            $pipe = [IO.Pipes.NamedPipeServerStream]::new(
                $Name, [IO.Pipes.PipeDirection]::InOut, 1,
                [IO.Pipes.PipeTransmissionMode]::Byte,
                [IO.Pipes.PipeOptions]::Asynchronous)
            try {
                $pipe.WaitForConnection()
                $handshake = Read-Frame $pipe
                if ($null -eq $handshake) { continue }
                if ($Mode -eq 'reject') {
                    Write-Frame $pipe 2 '{"evt":"ERROR","code":4000}'
                    continue
                }
                if ($Mode -eq 'delay') { Start-Sleep -Milliseconds 1000 }
                if ($Mode -eq 'malformed' -and $index -eq 0) {
                    Write-Frame $pipe 1 '{malformed'
                    continue
                }
                Write-Frame $pipe 1 '{"cmd":"DISPATCH","evt":"READY","data":{}}'
                $activity = Read-Frame $pipe
                if ($null -ne $activity) {
                    [pscustomobject]@{
                        connection = $index + 1
                        ticks = [Diagnostics.Stopwatch]::GetTimestamp()
                        utc = [DateTime]::UtcNow.ToString('o')
                        opcode = $activity.Opcode
                        json = $activity.Json
                    } | ConvertTo-Json -Compress |
                        Add-Content -LiteralPath $Log -Encoding UTF8
                    if ($Mode -ne 'close' -or $index -ne 0) {
                        $request = $activity.Json | ConvertFrom-Json
                        $ack = @{
                            cmd = 'SET_ACTIVITY'
                            data = $null
                            evt = $null
                            nonce = $request.nonce
                        } | ConvertTo-Json -Compress
                        Write-Frame $pipe 1 $ack
                    }
                }
                if ($Mode -ne 'close' -or $index -ne 0) {
                    Start-Sleep -Seconds 15
                }
            }
            finally { $pipe.Dispose() }
        }
    }
}

function Start-HttpFaultServer(
    [int]$Port, [string]$Mode, [string]$Log
) {
    Start-Job -ArgumentList $Port, $Mode, $Log -ScriptBlock {
        param($Port, $Mode, $Log)
        $listener = [Net.Sockets.TcpListener]::new(
            [Net.IPAddress]::Loopback, [int]$Port)
        $listener.Start()
        try {
            for ($requestIndex = 1; $requestIndex -le 2; ++$requestIndex) {
                $client = $listener.AcceptTcpClient()
                try {
                    $stream = $client.GetStream()
                    $headers = [Collections.Generic.List[byte]]::new()
                    $tail = [Collections.Generic.Queue[byte]]::new()
                    while ($headers.Count -lt 65536) {
                        $value = $stream.ReadByte()
                        if ($value -lt 0) { break }
                        $headers.Add([byte]$value)
                        $tail.Enqueue([byte]$value)
                        while ($tail.Count -gt 4) { [void]$tail.Dequeue() }
                        if ($tail.Count -eq 4 -and
                                ($tail.ToArray() -join ',') -eq '13,10,13,10') {
                            break
                        }
                    }
                    $text = [Text.Encoding]::ASCII.GetString($headers.ToArray())
                    $match = [regex]::Match($text, '(?im)^Content-Length:\s*(\d+)\s*$')
                    $length = if ($match.Success) { [int]$match.Groups[1].Value } else { 0 }
                    $body = [byte[]]::new($length)
                    $offset = 0
                    while ($offset -lt $length) {
                        $read = $stream.Read($body, $offset, $length - $offset)
                        if ($read -le 0) { break }
                        $offset += $read
                    }
                    [pscustomobject]@{
                        request = $requestIndex
                        ticks = [Diagnostics.Stopwatch]::GetTimestamp()
                        bytes = $offset
                    } | ConvertTo-Json -Compress |
                        Add-Content -LiteralPath $Log -Encoding UTF8
                    if ($Mode -eq 'http5xx') {
                        $response = [Text.Encoding]::ASCII.GetBytes(
                            "HTTP/1.1 503 Service Unavailable`r`nContent-Length: 0`r`nConnection: close`r`n`r`n")
                        $stream.Write($response, 0, $response.Length)
                    } else {
                        $bodyText = [Text.Encoding]::UTF8.GetBytes('not a valid url')
                        $response = [Text.Encoding]::ASCII.GetBytes(
                            "HTTP/1.1 200 OK`r`nContent-Type: text/plain`r`nContent-Length: $($bodyText.Length)`r`nConnection: close`r`n`r`n")
                        $stream.Write($response, 0, $response.Length)
                        $stream.Write($bodyText, 0, $bodyText.Length)
                    }
                    $stream.Flush()
                }
                finally { $client.Dispose() }
            }
        }
        finally { $listener.Stop() }
    }
}

function Wait-Options([string]$Pattern, [int]$TimeoutMs) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $state = Invoke-Probe 'options-state' -AllowFailure
        if ($state.Success -and $state.Output -match $Pattern) {
            return [pscustomobject]@{
                Output = $state.Output
                ElapsedMilliseconds = $watch.Elapsed.TotalMilliseconds
            }
        }
        Start-Sleep -Milliseconds 50
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw "Options did not match /$Pattern/ within ${TimeoutMs}ms. Last: $($state.Output)"
}

function Start-LibraryPlayback(
    [string]$PresenceLog,
    [string]$TrackFixture,
    [int]$TimeoutMs = 5000
) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        if ($TrackFixture) {
            Invoke-Probe 'drop-file' $TrackFixture -AllowFailure | Out-Null
        } else {
            Invoke-Probe 'lib' -AllowFailure | Out-Null
            Invoke-Probe 'play-first-album' -AllowFailure | Out-Null
        }
        Start-Sleep -Milliseconds 250
        $lines = @(if (Test-Path -LiteralPath $PresenceLog) {
            Get-Content -LiteralPath $PresenceLog
        })
        if ($lines -match 'publish .* state=1 ') {
            return $watch.Elapsed.TotalMilliseconds
        }
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw "Presence fixture did not start playback within ${TimeoutMs}ms."
}

function Stop-Candidate([Diagnostics.Process]$Process) {
    if ($null -eq $Process) { return }
    $Process.Refresh()
    if ($Process.HasExited) { return }
    & $appFull /exit | Out-Null
    if (-not $Process.WaitForExit(10000)) {
        Stop-Process -Id $Process.Id -Force
        throw 'Presence candidate required forced termination.'
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
        throw 'Multiple disposable Presence candidates are running.'
    }
    if ($matches.Count -eq 0) { return }
    & $appFull /exit | Out-Null
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        Start-Sleep -Milliseconds 50
        $alive = @(Get-Process -Id $matches[0].ProcessId -ErrorAction SilentlyContinue)
    } while ($alive.Count -ne 0 -and $watch.ElapsedMilliseconds -lt 10000)
    if ($alive.Count -ne 0) {
        throw 'Existing disposable Presence candidate did not exit cleanly.'
    }
}

$results = [Collections.Generic.List[object]]::new()
foreach ($mode in @('reject','delay','malformed','close','http5xx','invalidUrl')) {
    $pipeName = 'goden-presence-fault-' + [guid]::NewGuid().ToString('N')
    $rpcLog = Join-Path $root "$mode-rpc.jsonl"
    $presenceLog = Join-Path $root "$mode-presence.log"
    [IO.File]::WriteAllText($rpcLog, '')
    [IO.File]::WriteAllText($presenceLog, '')
    $rpcMode = if ($mode -in @('http5xx','invalidUrl')) { 'normal' } else { $mode }
    $rpcJob = Start-RpcServer $pipeName $rpcMode $rpcLog
    $httpJob = $null
    $httpLog = Join-Path $root "$mode-http.jsonl"
    $port = 0
    if ($mode -in @('http5xx','invalidUrl')) {
        [IO.File]::WriteAllText($httpLog, '')
        $port = New-Port
        $httpJob = Start-HttpFaultServer $port $mode $httpLog
    }
    $process = $null
    try {
        Stop-RunningCandidate
        $env:NEKOROX_QA_HOOKS = '1'
        $env:NEKOROX_QA_PROFILE = '1'
        $env:NEKOROX_QA_BACKGROUND = '1'
        $env:NEKOROX_QA_DISCORD_PIPE = $pipeName
        $env:NEKOROX_QA_PRESENCE_LOG = $presenceLog
        # This matrix exercises the retained IPC/Catbox-style rollback lane.
        # Presence V3 relay failures are covered by the relay's own live fault
        # integration test; point the candidate at an invalid endpoint here so
        # the local HTTP fault server below remains the active cover backend.
        $env:GODEN_PRESENCE_RELAY_URL = 'disabled'
        if ($port -ne 0) {
            $env:NEKOROX_QA_COVER_UPLOAD_URL = "http://127.0.0.1:$port/upload"
        }
        $process = Start-Process -FilePath $appFull -WindowStyle Hidden -PassThru
        foreach ($name in @('NEKOROX_QA_HOOKS','NEKOROX_QA_PROFILE',
                'NEKOROX_QA_BACKGROUND','NEKOROX_QA_DISCORD_PIPE',
                'NEKOROX_QA_PRESENCE_LOG','NEKOROX_QA_COVER_UPLOAD_URL',
                'GODEN_PRESENCE_RELAY_URL')) {
            Remove-Item "Env:$name" -ErrorAction SilentlyContinue
        }
        for ($attempt = 0; $attempt -lt 200; ++$attempt) {
            $window = Invoke-Probe 'window-state' -AllowFailure
            if ($window.Success) { break }
            Start-Sleep -Milliseconds 25
        }
        if (-not $window.Success) { throw "Presence candidate did not start ($mode)." }
        # The window handle is published before Columns UI finishes loading
        # the library model. Match the production QA harness and wait for that
        # one-time startup work before asking it to select an album.
        Start-Sleep -Seconds 1
        Invoke-Probe 'options-open' | Out-Null
        if ($mode -in @('http5xx','invalidUrl','delay')) {
            Invoke-Probe 'covers-enable' | Out-Null
        } else {
            Invoke-Probe 'covers-disable' | Out-Null
        }
        Invoke-Probe 'presence-enable' | Out-Null
        # Startup and a previous QA scenario can leave a non-Library tab while
        # Columns UI is still restoring its child tree. Confirm playback from
        # the service log instead of trusting a fixed post-click delay.
        $playbackStartMs = Start-LibraryPlayback $presenceLog $FixtureTrack

        $evidence = [ordered]@{
            mode = $mode
            playbackStartMilliseconds = $playbackStartMs
            passed = $true
        }
        if ($mode -eq 'reject') {
            $state = Wait-Options 'Discord rejected the application ID' 2500
            $evidence.statusMilliseconds = $state.ElapsedMilliseconds
        } elseif ($mode -eq 'delay') {
            $deadline = [Diagnostics.Stopwatch]::StartNew()
            do {
                $lines = @(if (Test-Path $presenceLog) {
                    Get-Content -LiteralPath $presenceLog
                })
                $readyIndex = -1
                $coverIndex = -1
                for ($index = 0; $index -lt $lines.Count; ++$index) {
                    if ($readyIndex -lt 0 -and $lines[$index] -match 'rpc ready') { $readyIndex = $index }
                    if ($coverIndex -lt 0 -and $lines[$index] -match 'cover resolve') { $coverIndex = $index }
                }
                if ($readyIndex -ge 0 -and $coverIndex -ge 0) { break }
                Start-Sleep -Milliseconds 50
            } while ($deadline.ElapsedMilliseconds -lt 4000)
            if ($readyIndex -lt 0 -or $coverIndex -le $readyIndex) {
                throw 'Rollback cover work did not remain behind Discord READY.'
            }
            $evidence.readyLogIndex = $readyIndex
            $evidence.coverLogIndex = $coverIndex
        } elseif ($mode -in @('malformed','close')) {
            $deadline = [Diagnostics.Stopwatch]::StartNew()
            do {
                $activities = @(Get-Content -LiteralPath $rpcLog |
                    Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
                if ($activities.Count -ge $(if ($mode -eq 'close') { 2 } else { 1 })) { break }
                Start-Sleep -Milliseconds 50
            } while ($deadline.ElapsedMilliseconds -lt 4500)
            $expected = if ($mode -eq 'close') { 2 } else { 1 }
            if ($activities.Count -lt $expected) {
                throw "RPC '$mode' did not reconnect/replay in time."
            }
            if ($mode -eq 'close') {
                $gapMs = ([double]$activities[1].ticks - [double]$activities[0].ticks) *
                    1000.0 / [Diagnostics.Stopwatch]::Frequency
                if ($gapMs -gt 2000.0) {
                    throw "RPC replay after close took ${gapMs}ms."
                }
                $evidence.replayMilliseconds = $gapMs
            } else {
                $evidence.recoveredMilliseconds = $deadline.Elapsed.TotalMilliseconds
            }
        } else {
            $state = Wait-Options 'cover fallback active' 13000
            $retryWait = [Diagnostics.Stopwatch]::StartNew()
            do {
                $requests = @(Get-Content -LiteralPath $httpLog |
                    Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
                if ($requests.Count -ge 2) { break }
                Start-Sleep -Milliseconds 50
            } while ($retryWait.ElapsedMilliseconds -lt 13000)
            if ($requests.Count -ne 2) {
                throw "HTTP '$mode' expected exactly one retry; got $($requests.Count) requests."
            }
            $retryMs = ([double]$requests[1].ticks - [double]$requests[0].ticks) *
                1000.0 / [Diagnostics.Stopwatch]::Frequency
            if ($retryMs -lt 900.0) {
                throw "HTTP '$mode' retry occurred before bounded backoff (${retryMs}ms)."
            }
            $evidence.retryMilliseconds = $retryMs
            $evidence.statusMilliseconds = $state.ElapsedMilliseconds
        }
        Invoke-Probe 'presence-disable' | Out-Null
        $results.Add([pscustomobject]$evidence)
    }
    finally {
        foreach ($name in @('NEKOROX_QA_HOOKS','NEKOROX_QA_PROFILE',
                'NEKOROX_QA_BACKGROUND','NEKOROX_QA_DISCORD_PIPE',
                'NEKOROX_QA_PRESENCE_LOG','NEKOROX_QA_COVER_UPLOAD_URL',
                'GODEN_PRESENCE_RELAY_URL')) {
            Remove-Item "Env:$name" -ErrorAction SilentlyContinue
        }
        Stop-Candidate $process
        foreach ($job in @($rpcJob, $httpJob) | Where-Object { $null -ne $_ }) {
            Remove-Job $job -Force -ErrorAction SilentlyContinue
        }
    }
}

$manifest = [ordered]@{
    schema = 'goden.presence-fault-matrix.v2'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    app = $appFull
    passed = $true
    scenarios = @($results)
}
$manifest | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath $manifestFull -Encoding UTF8
Write-Host 'Goden Discord Presence fault matrix: PASS'
Write-Host "Evidence: $manifestFull"
