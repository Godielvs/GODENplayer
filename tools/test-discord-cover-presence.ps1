param(
    [Parameter(Mandatory = $true)]
    [string]$App,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$RealCoverHost,
    [switch]$StallCoverHost,
    [switch]$DelayCoverReadiness,
    [switch]$Background,
    [string]$FixturePath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($RealCoverHost -and $StallCoverHost) {
    throw 'RealCoverHost and StallCoverHost are mutually exclusive.'
}
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$appFull = [IO.Path]::GetFullPath($App)
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
$relayUrl = $env:GODEN_PRESENCE_RELAY_URL
if (-not $relayUrl) {
    $deploymentHeader = Join-Path $repo 'src\component\presence_deployment.h'
    if (Test-Path -LiteralPath $deploymentHeader -PathType Leaf) {
        $deploymentText = Get-Content -LiteralPath $deploymentHeader -Raw
        if ($deploymentText -match 'relayEndpoint\{"([^"]+)"\}') {
            $relayUrl = $Matches[1]
        }
    }
}
foreach ($required in @($appFull, $probe)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Discord cover QA input is missing: $required"
    }
}
if ($FixturePath) {
    $FixturePath = [IO.Path]::GetFullPath($FixturePath)
    if (-not (Test-Path -LiteralPath $FixturePath)) {
        throw "Discord cover fixture is missing: $FixturePath"
    }
}

function Invoke-Probe([string]$Action, [string]$Argument = '') {
    $arguments = @($appFull, $Action)
    if ($Argument) { $arguments += $Argument }
    if ($Background) { $arguments += '--background' }
    $output = & $probe @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "Discord QA probe '$Action' failed: $($output -join "`n")"
    }
    return $output
}

$qaRoot = Join-Path $repo 'artifacts\discord-cover-qa'
[IO.Directory]::CreateDirectory($qaRoot) | Out-Null
$pipeName = 'goden-cover-qa-' + [guid]::NewGuid().ToString('N')
$rpcLog = Join-Path $qaRoot 'rpc.jsonl'
$rpcError = Join-Path $qaRoot 'rpc-error.txt'
$presenceLog = Join-Path $qaRoot 'presence.log'
$httpBody = Join-Path $qaRoot 'upload.bin'
[IO.File]::WriteAllText($rpcLog, '')
[IO.File]::WriteAllText($rpcError, '')
[IO.File]::WriteAllText($presenceLog, '')
[IO.File]::WriteAllBytes($httpBody, [byte[]]@())

$portProbe = [Net.Sockets.TcpListener]::new(
    [Net.IPAddress]::Loopback, 0)
$portProbe.Start()
$port = ([Net.IPEndPoint]$portProbe.LocalEndpoint).Port
$portProbe.Stop()
$coverUrl = "http://127.0.0.1:$port/cover.png"
$uploadUrl = "http://127.0.0.1:$port/upload"

$rpcJob = Start-Job -ArgumentList $pipeName, $rpcLog, $rpcError -ScriptBlock {
    param($Name, $Log, $ErrorLog)
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
    function Write-Frame([IO.Stream]$Stream, [uint32]$Opcode, [string]$Json) {
        $payload = [Text.Encoding]::UTF8.GetBytes($Json)
        $header = [byte[]]::new(8)
        [BitConverter]::GetBytes($Opcode).CopyTo($header, 0)
        [BitConverter]::GetBytes([uint32]$payload.Length).CopyTo($header, 4)
        $Stream.Write($header, 0, $header.Length)
        $Stream.Write($payload, 0, $payload.Length)
        $Stream.Flush()
    }
    $pipe = [IO.Pipes.NamedPipeServerStream]::new(
        $Name, [IO.Pipes.PipeDirection]::InOut, 1,
        [IO.Pipes.PipeTransmissionMode]::Byte,
        [IO.Pipes.PipeOptions]::Asynchronous)
    try {
        $pipe.WaitForConnection()
        while ($pipe.IsConnected) {
            $header = Read-Exact $pipe 8
            if ($null -eq $header) { break }
            $opcode = [BitConverter]::ToUInt32($header, 0)
            $length = [BitConverter]::ToUInt32($header, 4)
            if ($length -gt 1048576) { break }
            $bytes = Read-Exact $pipe ([int]$length)
            if ($null -eq $bytes) { break }
            $json = [Text.Encoding]::UTF8.GetString($bytes)
            if ($opcode -eq 0) {
                Write-Frame $pipe 1 '{"cmd":"DISPATCH","evt":"READY","data":{"config":{"cdn_host":"cdn.discordapp.com"}}}'
            }
            elseif ($opcode -eq 1) {
                [IO.File]::AppendAllText($Log, $json + [Environment]::NewLine)
                $request = $json | ConvertFrom-Json
                $ack = @{
                    cmd = 'SET_ACTIVITY'
                    data = $null
                    evt = $null
                    nonce = $request.nonce
                } | ConvertTo-Json -Compress
                Write-Frame $pipe 1 $ack
            }
            elseif ($opcode -eq 3) {
                Write-Frame $pipe 4 $json
            }
        }
    }
    catch {
        [IO.File]::WriteAllText($ErrorLog, ($_ | Out-String))
        throw
    }
    finally {
        $pipe.Dispose()
    }
}

$httpJob = if (-not $RealCoverHost) { Start-Job -ArgumentList $port, $httpBody, $coverUrl, ([bool]$StallCoverHost), ([bool]$DelayCoverReadiness) -ScriptBlock {
    param($Port, $Output, $CoverUrl, $Stall, $DelayReadiness)
    $listener = [Net.Sockets.TcpListener]::new(
        [Net.IPAddress]::Loopback, [int]$Port)
    $listener.Start()
    try {
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $headerBytes = [Collections.Generic.List[byte]]::new()
            $window = [Collections.Generic.Queue[byte]]::new()
            while ($headerBytes.Count -lt 65536) {
                $value = $stream.ReadByte()
                if ($value -lt 0) { throw 'HTTP request ended before headers' }
                $byte = [byte]$value
                $headerBytes.Add($byte)
                $window.Enqueue($byte)
                while ($window.Count -gt 4) { [void]$window.Dequeue() }
                if ($window.Count -eq 4 -and
                    ($window.ToArray() -join ',') -eq '13,10,13,10') { break }
            }
            $headerText = [Text.Encoding]::ASCII.GetString(
                $headerBytes.ToArray())
            $match = [regex]::Match(
                $headerText, '(?im)^Content-Length:\s*(\d+)\s*$')
            if (-not $match.Success) { throw 'Missing Content-Length' }
            $length = [int]$match.Groups[1].Value
            $body = [byte[]]::new($length)
            $offset = 0
            while ($offset -lt $length) {
                $read = $stream.Read($body, $offset, $length - $offset)
                if ($read -le 0) { throw 'HTTP upload ended early' }
                $offset += $read
            }
            [IO.File]::WriteAllBytes($Output, $body)
            if ($Stall) {
                Start-Sleep -Seconds 60
                return
            }
            $response = [Text.Encoding]::UTF8.GetBytes($CoverUrl)
            $headers = [Text.Encoding]::ASCII.GetBytes(
                "HTTP/1.1 200 OK`r`nContent-Type: text/plain`r`nContent-Length: $($response.Length)`r`nConnection: close`r`n`r`n")
            $stream.Write($headers, 0, $headers.Length)
            $stream.Write($response, 0, $response.Length)
            $stream.Flush()

            # Production does not publish an upload URL until a GET confirms
            # that the object is already readable. Model that propagation
            # contract explicitly instead of letting the fake host make an
            # unverified URL look ready.
            $verifyAttempts = if ($DelayReadiness) { 2 } else { 1 }
            for ($verifyIndex = 1; $verifyIndex -le $verifyAttempts;
                    ++$verifyIndex) {
                $verifyClient = $listener.AcceptTcpClient()
                try {
                    $verifyStream = $verifyClient.GetStream()
                    $verifyWindow = [Collections.Generic.Queue[byte]]::new()
                    while ($true) {
                        $value = $verifyStream.ReadByte()
                        if ($value -lt 0) {
                            throw 'Verification GET ended early'
                        }
                        $verifyWindow.Enqueue([byte]$value)
                        while ($verifyWindow.Count -gt 4) {
                            [void]$verifyWindow.Dequeue()
                        }
                        if ($verifyWindow.Count -eq 4 -and
                            ($verifyWindow.ToArray() -join ',') -eq
                                '13,10,13,10') { break }
                    }
                    if ($DelayReadiness -and $verifyIndex -eq 1) {
                        $notReady = [Text.Encoding]::ASCII.GetBytes(
                            "HTTP/1.1 404 Not Found`r`nContent-Length: 0`r`nConnection: close`r`n`r`n")
                        $verifyStream.Write($notReady, 0, $notReady.Length)
                    } else {
                        $png = [byte[]](137,80,78,71,13,10,26,10)
                        $verifyHeaders = [Text.Encoding]::ASCII.GetBytes(
                            "HTTP/1.1 200 OK`r`nContent-Type: image/png`r`nContent-Length: $($png.Length)`r`nConnection: close`r`n`r`n")
                        $verifyStream.Write(
                            $verifyHeaders, 0, $verifyHeaders.Length)
                        $verifyStream.Write($png, 0, $png.Length)
                    }
                    $verifyStream.Flush()
                }
                finally { $verifyClient.Dispose() }
            }
        }
        finally { $client.Dispose() }
    }
    finally { $listener.Stop() }
} } else { $null }

$process = $null
try {
    $existing = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and [IO.Path]::GetFullPath(
                $_.ExecutablePath).Equals(
                    $appFull, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($existing.Count -gt 1) {
        throw 'Multiple disposable Discord cover candidates are running.'
    }
    if ($existing.Count -eq 1) {
        & $appFull /exit | Out-Null
        $watch = [Diagnostics.Stopwatch]::StartNew()
        do {
            Start-Sleep -Milliseconds 50
            $alive = @(Get-Process -Id $existing[0].ProcessId `
                -ErrorAction SilentlyContinue)
        } while ($alive.Count -ne 0 -and $watch.ElapsedMilliseconds -lt 10000)
        if ($alive.Count -ne 0) {
            throw 'Existing disposable Discord cover candidate did not exit.'
        }
    }
    $env:NEKOROX_QA_HOOKS = '1'
    $env:NEKOROX_QA_PROFILE = '1'
    if ($Background) { $env:NEKOROX_QA_BACKGROUND = '1' }
    $env:NEKOROX_QA_DISCORD_PIPE = $pipeName
    if (-not $RealCoverHost) {
        $env:NEKOROX_QA_COVER_UPLOAD_URL = $uploadUrl
    }
    $env:NEKOROX_QA_PRESENCE_LOG = $presenceLog
    $process = if ($Background) {
        Start-Process -FilePath $appFull -WindowStyle Hidden -PassThru
    } else {
        Start-Process -FilePath $appFull -PassThru
    }
    Remove-Item Env:NEKOROX_QA_DISCORD_PIPE -ErrorAction SilentlyContinue
    Remove-Item Env:NEKOROX_QA_COVER_UPLOAD_URL -ErrorAction SilentlyContinue
    Remove-Item Env:NEKOROX_QA_PRESENCE_LOG -ErrorAction SilentlyContinue
    Remove-Item Env:NEKOROX_QA_HOOKS -ErrorAction SilentlyContinue
    Remove-Item Env:NEKOROX_QA_PROFILE -ErrorAction SilentlyContinue
    Remove-Item Env:NEKOROX_QA_BACKGROUND -ErrorAction SilentlyContinue
    for ($attempt = 0; $attempt -lt 80; $attempt++) {
        $process.Refresh()
        if ($process.HasExited) { throw 'Disposable Goden exited during startup' }
        if ($process.Responding -and $process.MainWindowHandle -ne 0) { break }
        Start-Sleep -Milliseconds 250
    }

    Start-Sleep -Seconds 1
    Invoke-Probe 'options-open' | Out-Null
    Invoke-Probe 'covers-enable' | Out-Null
    Invoke-Probe 'presence-enable' | Out-Null
    if ($FixturePath) {
        Invoke-Probe 'drop-file' $FixturePath | Out-Null
    } else {
        # Columns UI publishes its top-level HWND before the restored Library
        # model is necessarily selectable. Retry the semantic action until the
        # playback callback confirms it, instead of trusting a fixed startup
        # delay and occasionally testing an idle player.
        $playbackDeadline = [Diagnostics.Stopwatch]::StartNew()
        do {
            Invoke-Probe 'lib' | Out-Null
            Invoke-Probe 'play-first-album' | Out-Null
            Start-Sleep -Milliseconds 250
            $presenceText = if (Test-Path -LiteralPath $presenceLog) {
                [IO.File]::ReadAllText($presenceLog)
            } else { '' }
            if ($presenceText -match 'publish .* state=1') { break }
            Start-Sleep -Milliseconds 500
        } while ($playbackDeadline.ElapsedMilliseconds -lt 6000)
        if ($presenceText -notmatch 'publish .* state=1') {
            throw 'Library playback did not start for the cover Presence test.'
        }
    }

    if ($StallCoverHost) {
        for ($attempt = 0; $attempt -lt 80; $attempt++) {
            Start-Sleep -Milliseconds 100
            if ((Get-Item $httpBody).Length -gt 0) { break }
        }
        if ((Get-Item $httpBody).Length -eq 0) {
            throw 'Stalled host did not receive the cover upload.'
        }
        $timer = [Diagnostics.Stopwatch]::StartNew()
        Invoke-Probe 'presence-disable' | Out-Null
        $timer.Stop()
        if ($timer.Elapsed.TotalMilliseconds -gt 500) {
            throw "Cancelling a stalled cover upload took $($timer.Elapsed.TotalMilliseconds) ms"
        }
        Invoke-Probe 'stop' | Out-Null
        return [pscustomobject]@{
            stalledUploadBytes = (Get-Item $httpBody).Length
            presenceDisableMs = [Math]::Round($timer.Elapsed.TotalMilliseconds, 1)
            cancelled = $true
        } | ConvertTo-Json
    }

    $payload = $null
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($process.HasExited) {
            throw "Disposable Goden exited with code $($process.ExitCode)"
        }
        foreach ($line in [IO.File]::ReadAllLines($rpcLog)) {
            try {
                $candidate = $line | ConvertFrom-Json
                $image = $candidate.args.activity.assets.large_image
                if ((-not $RealCoverHost -and $image -eq $coverUrl) -or
                    ($RealCoverHost -and (
                        $image.StartsWith('https://wsrv.nl/?url=https%3A%2F%2Ffiles.catbox.moe%2F') -or
                        ($relayUrl -and
                            $image.StartsWith($relayUrl.TrimEnd('/') + '/v1/c/'))))) {
                    $payload = $candidate
                }
            } catch {}
        }
        if ($null -ne $payload) { break }
    }
    if ($null -eq $payload) {
        $diagnosticState = Invoke-Probe 'options-state'
        $rpcFailure = [IO.File]::ReadAllText($rpcError)
        $presenceDiagnostic = [IO.File]::ReadAllText($presenceLog)
        throw ('Discord RPC never received the temporary local cover URL. ' +
            "Options=[$diagnosticState] RPC job=$($rpcJob.State) " +
            "HTTP bytes=$((Get-Item $httpBody).Length) error=[$rpcFailure] " +
            "Presence log=[$presenceDiagnostic]")
    }
    $largeTextProperty = $payload.args.activity.assets.PSObject.Properties[
        'large_text']
    if ($null -ne $largeTextProperty -and
            $largeTextProperty.Value -eq 'GODENplayer') {
        throw 'Discord activity redundantly repeats GODENplayer inside its card.'
    }
    $state = Invoke-Probe 'options-state'
    if ($state -notmatch 'presence=1' -or $state -notmatch 'covers=1') {
        throw "Unexpected Options state: $state"
    }

    $body = [byte[]]@()
    if (-not $RealCoverHost) {
        $body = [IO.File]::ReadAllBytes($httpBody)
        $ascii = [Text.Encoding]::ASCII.GetString($body)
        if ($ascii -notmatch 'name="reqtype"' -or
            $ascii -notmatch 'name="fileToUpload"' -or
            $ascii -notmatch 'Content-Type: image/png') {
            throw 'Temporary cover multipart body is incomplete.'
        }
        $pngSignature = [byte[]](137,80,78,71,13,10,26,10)
        $foundPng = $false
        for ($index = 0; $index -le $body.Length - $pngSignature.Length; $index++) {
            $matches = $true
            for ($inner = 0; $inner -lt $pngSignature.Length; $inner++) {
                if ($body[$index + $inner] -ne $pngSignature[$inner]) {
                    $matches = $false; break
                }
            }
            if ($matches) { $foundPng = $true; break }
        }
        if (-not $foundPng) { throw 'Uploaded cover is not a valid PNG stream.' }
    }

    $cacheReused = $null
    if (-not $RealCoverHost) {
        $firstDetails = $payload.args.activity.details
        $lineCountBeforeNext = [IO.File]::ReadAllLines($rpcLog).Length
        Invoke-Probe 'next' | Out-Null
        $cachePayload = $null
        for ($attempt = 0; $attempt -lt 80; $attempt++) {
            Start-Sleep -Milliseconds 150
            foreach ($line in [IO.File]::ReadAllLines($rpcLog)) {
                try {
                    $candidate = $line | ConvertFrom-Json
                    if ($candidate.args.activity.details -ne $firstDetails -and
                        $candidate.args.activity.assets.large_image -eq $coverUrl) {
                        $cachePayload = $candidate
                    }
                } catch {}
            }
            if ($null -ne $cachePayload) { break }
        }
        if ($null -eq $cachePayload) {
            throw 'The next track did not reuse its album cover URL cache.'
        }
        $transitionLines = [IO.File]::ReadAllLines($rpcLog)
        if ($transitionLines.Length -gt $lineCountBeforeNext) {
            foreach ($line in $transitionLines[
                    $lineCountBeforeNext..($transitionLines.Length - 1)]) {
                try {
                    $candidate = $line | ConvertFrom-Json
                    if ($candidate.cmd -eq 'SET_ACTIVITY' -and
                            $null -eq $candidate.args.activity) {
                        throw 'Discord activity was cleared during an ordinary track transition.'
                    }
                }
                catch {
                    if ($_.Exception.Message -eq
                            'Discord activity was cleared during an ordinary track transition.') {
                        throw
                    }
                }
            }
        }
        $cacheReused = $true
    }

    # Disabling covers must retire the already-published temporary URL for the
    # current track; otherwise it would remain visible until a track change.
    $lineCountBeforeDisable = [IO.File]::ReadAllLines($rpcLog).Length
    Invoke-Probe 'covers-disable' | Out-Null
    $coverDisableCleared = $false
    for ($attempt = 0; $attempt -lt 80; $attempt++) {
        Start-Sleep -Milliseconds 150
        $lines = [IO.File]::ReadAllLines($rpcLog)
        if ($lines.Length -le $lineCountBeforeDisable) { continue }
        foreach ($line in $lines[$lineCountBeforeDisable..($lines.Length - 1)]) {
            try {
                $candidate = $line | ConvertFrom-Json
                $assets = $candidate.args.activity.assets
                $imageProperty = $assets.PSObject.Properties['large_image']
                $image = if ($null -ne $imageProperty) {
                    $imageProperty.Value
                } else { $null }
                if ($image -ne $payload.args.activity.assets.large_image) {
                    $coverDisableCleared = $true
                }
            } catch {}
        }
        if ($coverDisableCleared) { break }
    }
    if (-not $coverDisableCleared) {
        throw 'Disabling temporary covers did not retire the current cover URL.'
    }
    Invoke-Probe 'covers-enable' | Out-Null

    $timer = [Diagnostics.Stopwatch]::StartNew()
    Invoke-Probe 'presence-disable' | Out-Null
    $timer.Stop()
    Invoke-Probe 'stop' | Out-Null
    [pscustomobject]@{
        cover = $payload.args.activity.assets.large_image
        uploadBytes = $body.Length
        cacheReused = $cacheReused
        coverDisableCleared = $coverDisableCleared
        redundantBrandingRemoved = $true
        presenceDisableMs = [Math]::Round($timer.Elapsed.TotalMilliseconds, 1)
        options = $state
    } | ConvertTo-Json -Depth 4
}
finally {
    Remove-Item Env:NEKOROX_QA_HOOKS -ErrorAction SilentlyContinue
    Remove-Item Env:NEKOROX_QA_PROFILE -ErrorAction SilentlyContinue
    Remove-Item Env:NEKOROX_QA_BACKGROUND -ErrorAction SilentlyContinue
    Remove-Item Env:NEKOROX_QA_DISCORD_PIPE -ErrorAction SilentlyContinue
    Remove-Item Env:NEKOROX_QA_COVER_UPLOAD_URL -ErrorAction SilentlyContinue
    Remove-Item Env:NEKOROX_QA_PRESENCE_LOG -ErrorAction SilentlyContinue
    if ($null -ne $process -and -not $process.HasExited) {
        & $appFull /exit | Out-Null
        if (-not $process.WaitForExit(5000)) {
            Stop-Process -Id $process.Id -Force
        }
    }
    foreach ($job in @($rpcJob, $httpJob) | Where-Object { $null -ne $_ }) {
        Remove-Job $job -Force -ErrorAction SilentlyContinue
    }
}
