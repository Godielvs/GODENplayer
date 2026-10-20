param(
    [Parameter(Mandatory=$true)][string]$App,
    [Parameter(Mandatory=$true)][string]$EvidenceRoot,
    [Parameter(Mandatory=$true)][string]$FixtureTrack,
    [ValidateSet('clearFailure','clearSuccess','enabledFailure')][string]$Scenario='clearFailure',
    [ValidateRange(0,1)][int]$ExpectedReconnect=0,
    [ValidateRange(0,1)][int]$ExpectedClear=1,
    [ValidateRange(0,2000)][int]$DisableAfterAckMs=0,
    [switch]$ReEnableAfterClear
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$appFull=[IO.Path]::GetFullPath($App)
$root=[IO.Path]::GetFullPath($EvidenceRoot)
$fixture=[IO.Path]::GetFullPath($FixtureTrack)
$probe=Join-Path $repo 'out/Release/nekorox_ui_probe.exe'
$allowed=[IO.Path]::GetFullPath((Join-Path $repo 'artifacts'))+[IO.Path]::DirectorySeparatorChar
if(-not $appFull.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase) -or
   -not $root.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)) {throw 'Only disposable artifact apps/evidence are allowed.'}
foreach($p in @($appFull,$fixture,$probe)){if(-not(Test-Path -LiteralPath $p -PathType Leaf)){throw "Missing $p"}}
if(Test-Path -LiteralPath $root){throw 'Use a new evidence directory; never overwrite an attempt.'}
New-Item -ItemType Directory $root | Out-Null
$log=Join-Path $root 'rpc.jsonl'
$resultPath=Join-Path $root 'server-result.json'
$commands=[Collections.Generic.List[object]]::new()
function Invoke-Probe([string]$Action,[string]$Argument='', [switch]$AllowFailure){
    $arguments=@($appFull,$Action);if($Argument){$arguments+=$Argument};$arguments+='--background'
    $old=$ErrorActionPreference
    $startTicks=[Diagnostics.Stopwatch]::GetTimestamp()
    try{$ErrorActionPreference='Continue';$output=& $probe @arguments 2>&1;$code=$LASTEXITCODE}
    finally{$ErrorActionPreference=$old}
    $commands.Add([pscustomobject]@{action=$Action;exit=$code;output=($output -join "`n");startTicks=$startTicks;ticks=[Diagnostics.Stopwatch]::GetTimestamp()})
    if($code -ne 0 -and -not $AllowFailure){throw "Probe $Action failed: $output"}
    return $code -eq 0
}
function Read-Events {
    if(-not(Test-Path -LiteralPath $log)){return @()}
    $stream=[IO.FileStream]::new($log,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    try{$reader=[IO.StreamReader]::new($stream);$text=$reader.ReadToEnd()}finally{$stream.Dispose()}
    return @($text -split "`n" | Where-Object {$_} | ForEach-Object {try{$_|ConvertFrom-Json}catch{}})
}
$pipeName='goden-presence-optout-'+[guid]::NewGuid().ToString('N')
$job=Start-Job -ArgumentList $pipeName,$Scenario,$log,$resultPath -ScriptBlock {
    param($Name,$Mode,$Log,$Result)
    $ErrorActionPreference='Stop'
    $logStream=[IO.FileStream]::new($Log,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite)
    $writer=[IO.StreamWriter]::new($logStream,[Text.UTF8Encoding]::new($false));$writer.AutoFlush=$true
    function Event([string]$Kind,$Data=$null){
        $writer.WriteLine(([pscustomobject]@{kind=$Kind;ticks=[Diagnostics.Stopwatch]::GetTimestamp();data=$Data}|ConvertTo-Json -Depth 12 -Compress))
    }
    function Read-Exact([IO.Stream]$s,[int]$n){
        $bytes=[byte[]]::new($n);$offset=0
        while($offset -lt $n){$count=$s.Read($bytes,$offset,$n-$offset);if($count -le 0){return $null};$offset+=$count}
        return ,$bytes
    }
    function Read-Frame([IO.Stream]$s){
        $h=Read-Exact $s 8;if($null -eq $h){return $null}
        $n=[BitConverter]::ToUInt32($h,4);if($n -gt 1048576){throw 'Frame too large'}
        $body=Read-Exact $s $n;if($null -eq $body){return $null}
        return [pscustomobject]@{opcode=[BitConverter]::ToUInt32($h,0);json=[Text.Encoding]::UTF8.GetString($body)}
    }
    function Write-Frame([IO.Stream]$s,[string]$text){
        $b=[Text.Encoding]::UTF8.GetBytes($text);$h=[byte[]]::new(8)
        [BitConverter]::GetBytes([uint32]1).CopyTo($h,0);[BitConverter]::GetBytes([uint32]$b.Length).CopyTo($h,4)
        $s.Write($h,0,8);$s.Write($b,0,$b.Length);$s.Flush()
    }
    function Ack($s,$request){Write-Frame $s (@{cmd='SET_ACTIVITY';data=$null;evt=$null;nonce=$request.nonce}|ConvertTo-Json -Compress)}
    function New-Pipe {return [IO.Pipes.NamedPipeServerStream]::new($Name,[IO.Pipes.PipeDirection]::InOut,2,[IO.Pipes.PipeTransmissionMode]::Byte,[IO.Pipes.PipeOptions]::Asynchronous)}
    $first=$null;$second=$null
    try{
        $first=New-Pipe;Event 'listening';$first.WaitForConnection();Event 'connection' 1
        $hello=Read-Frame $first;if($null -eq $hello){throw 'No first handshake'};Event 'handshake' 1
        Write-Frame $first '{"cmd":"DISPATCH","evt":"READY","data":{}}'
        $played=$false;$faultMs=0.;$faultObserved=$false
        while($first.IsConnected){
            $f=Read-Frame $first;if($null -eq $f){break};if($f.opcode -ne 1){continue}
            $r=$f.json|ConvertFrom-Json;Event 'activity' @{connection=1;payload=$r}
            $clear=$null -eq $r.args.activity
            if($played -and $clear -and $Mode -eq 'clearSuccess'){
                $second=New-Pipe;$pending=$second.BeginWaitForConnection($null,$null)
                Ack $first $r;Event 'clearAck';$faultObserved=$true
                # Wait for the real drain to close the existing connection.
                $closed=Read-Frame $first;if($null -ne $closed){throw 'Unexpected frame after successful clear'}
                break
            }
            if(($played -and $clear -and $Mode -eq 'clearFailure') -or
               (-not $clear -and $Mode -eq 'enabledFailure')){
                if(-not $clear){Ack $first $r;Event 'playingAck';$played=$true}
                $second=New-Pipe;$pending=$second.BeginWaitForConnection($null,$null)
                Event 'faultArmed' $Mode;$watch=[Diagnostics.Stopwatch]::StartNew()
                # Policy is already disabled when the null clear is observed.
                # Fail within the 475 ms asynchronous drain grace interval.
                Start-Sleep -Milliseconds 120
                $faultMs=$watch.Elapsed.TotalMilliseconds;$faultObserved=$true
                Event 'pipeClose' $faultMs
                break
            }
            Ack $first $r
            if(-not $clear){$played=$true;Event 'playingAck'}
        }
        $first.Dispose();$first=$null
        if($null -eq $second){$second=New-Pipe;$pending=$second.BeginWaitForConnection($null,$null)}
        $reconnected=$pending.AsyncWaitHandle.WaitOne(1500)
        $replayed=$false;$secondHandshake=$false
        if($reconnected){
            $second.EndWaitForConnection($pending);Event 'connection' 2
            $hello=Read-Frame $second
            if($null -ne $hello){
                $secondHandshake=$true;Event 'handshake' 2
                Write-Frame $second '{"cmd":"DISPATCH","evt":"READY","data":{}}'
                $f=Read-Frame $second
                if($null -ne $f){$r=$f.json|ConvertFrom-Json;Event 'activity' @{connection=2;payload=$r};Ack $second $r;$replayed=$null -ne $r.args.activity}
            }
        }
        Event 'observationEnd'
        [ordered]@{scenario=$Mode;played=$played;faultObserved=$faultObserved;faultDelayMs=$faultMs;reconnected=$reconnected;secondHandshake=$secondHandshake;replayed=$replayed;observationMs=1500}|ConvertTo-Json|Set-Content -LiteralPath $Result -Encoding UTF8
    }catch{[IO.File]::WriteAllText($Result+'.error.txt',($_|Out-String));try{Event 'serverError' $_.ToString()}catch{};throw}
    finally{if($null -ne $first){$first.Dispose()};if($null -ne $second){$second.Dispose()};$writer.Dispose();$logStream.Dispose()}
}
$process=$null;$saved=@{}
$envNames=@('NEKOROX_QA_HOOKS','NEKOROX_QA_PROFILE','NEKOROX_QA_BACKGROUND','NEKOROX_QA_DISCORD_PIPE','NEKOROX_QA_PRESENCE_LOG','GODEN_FLAG_PRESENCE_EPHEMERAL_RELAY_V1','GODEN_FLAG_PRESENCE_REVISION_V2')
try{
    foreach($n in $envNames){$saved[$n]=[Environment]::GetEnvironmentVariable($n,'Process')}
    $existing=@(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'"|Where-Object {$_.ExecutablePath -and [IO.Path]::GetFullPath($_.ExecutablePath).Equals($appFull,[StringComparison]::OrdinalIgnoreCase)})
    if($existing.Count){throw 'Disposable target is already running'}
    $env:NEKOROX_QA_HOOKS='1';$env:NEKOROX_QA_PROFILE='1';$env:NEKOROX_QA_BACKGROUND='1'
    $env:NEKOROX_QA_DISCORD_PIPE=$pipeName;$env:NEKOROX_QA_PRESENCE_LOG=Join-Path $root 'presence.log'
    $env:GODEN_FLAG_PRESENCE_EPHEMERAL_RELAY_V1='0';$env:GODEN_FLAG_PRESENCE_REVISION_V2='1'
    $timer=[Diagnostics.Stopwatch]::StartNew()
    while(-not (@(Read-Events | Where-Object kind -eq 'listening').Count -gt 0)){if($timer.ElapsedMilliseconds -gt 8000){throw 'Fake server not ready'};Start-Sleep -Milliseconds 25}
    $process=Start-Process -FilePath $appFull -WindowStyle Hidden -PassThru
    foreach($n in $envNames){[Environment]::SetEnvironmentVariable($n,$saved[$n],'Process')}
    $timer.Restart();$ready=$false
    while(-not $ready){$ready=Invoke-Probe 'window-state' -AllowFailure;if($timer.ElapsedMilliseconds -gt 8000){throw 'App not ready'};Start-Sleep -Milliseconds 25}
    Invoke-Probe 'options-open'|Out-Null;Invoke-Probe 'covers-disable'|Out-Null;Invoke-Probe 'presence-enable'|Out-Null
    Invoke-Probe 'drop-file' $fixture|Out-Null
    $timer.Restart()
    while(-not (@(Read-Events | Where-Object kind -eq 'playingAck').Count -gt 0)){if($timer.ElapsedMilliseconds -gt 8000){throw 'No playing activity'};Start-Sleep -Milliseconds 25}
    if($Scenario -ne 'enabledFailure'){
        if($DisableAfterAckMs){Start-Sleep -Milliseconds $DisableAfterAckMs}
        Invoke-Probe 'presence-disable'|Out-Null
        if($ReEnableAfterClear){
            $timer.Restart()
            while(-not (@(Read-Events | Where-Object kind -eq 'faultArmed').Count -gt 0)){
                if($timer.ElapsedMilliseconds -gt 1000){throw 'Clear was not observed before re-enable'}
                Start-Sleep -Milliseconds 10
            }
            Invoke-Probe 'presence-enable'|Out-Null
        }
    }
    $timer.Restart()
    while(-not(Test-Path -LiteralPath $resultPath)){if($timer.ElapsedMilliseconds -gt 6000){throw 'No server result'};if($job.State -eq 'Failed'){Receive-Job $job;throw 'Server failed'};Start-Sleep -Milliseconds 25}
    $result=Get-Content -LiteralPath $resultPath -Raw|ConvertFrom-Json
    if([int]$result.reconnected -ne $ExpectedReconnect){throw "Reconnect expected=$ExpectedReconnect actual=$($result.reconnected)"}
    if(-not $result.played){throw 'No initial playing activity was acknowledged'}
    if([int]$result.faultObserved -ne $ExpectedClear){throw "Clear/fault expected=$ExpectedClear actual=$($result.faultObserved)"}
    if($Scenario -eq 'clearFailure' -and ($result.faultDelayMs -lt 100 -or $result.faultDelayMs -gt 350)){throw 'Fault missed the intended drain window'}
    $timing=[ordered]@{}
    if($Scenario -ne 'enabledFailure'){
        $disable=($commands | Where-Object action -eq 'presence-disable' | Select-Object -Last 1)
        $ack=(Read-Events | Where-Object kind -eq 'playingAck' | Select-Object -First 1).ticks
        $timing.ackToDisableStartMs=1000.0*($disable.startTicks-$ack)/[Diagnostics.Stopwatch]::Frequency
        $timing.ackToDisableReturnMs=1000.0*($disable.ticks-$ack)/[Diagnostics.Stopwatch]::Frequency
        if($DisableAfterAckMs -eq 0 -and $timing.ackToDisableReturnMs -gt 150){throw 'Immediate opt-out missed the required early window'}
        if($Scenario -eq 'clearFailure'){
            $close=(Read-Events | Where-Object kind -eq 'pipeClose' | Select-Object -First 1).ticks
            $timing.disableStartToFaultMs=1000.0*($close-$disable.startTicks)/[Diagnostics.Stopwatch]::Frequency
            if($timing.disableStartToFaultMs -ge 475){throw 'Pipe failed after the asynchronous drain grace window'}
        }
    }
    if($Scenario -eq 'enabledFailure' -and (-not $result.secondHandshake -or -not $result.replayed)){throw 'Enabled reconnection failed to replay activity'}
    if($ReEnableAfterClear){
        if(-not $result.secondHandshake -or -not $result.replayed){throw 'Re-enable failed to replay activity'}
        $enableTick=($commands | Where-Object action -eq 'presence-enable' | Select-Object -Last 1).ticks
        $closeTick=(Read-Events | Where-Object kind -eq 'pipeClose' | Select-Object -First 1).ticks
        if($enableTick -ge $closeTick){throw 'Re-enable missed the controlled drain window'}
    }
    Invoke-Probe 'options-state'|Out-Null
    [ordered]@{passed=$true;app=$appFull;coreSha256=(Get-FileHash (Join-Path (Split-Path $appFull) 'profile/user-components-x64/foo_nekorox/foo_nekorox.dll')).Hash;expectedReconnect=$ExpectedReconnect;expectedClear=$ExpectedClear;disableAfterAckMs=$DisableAfterAckMs;reEnableAfterClear=[bool]$ReEnableAfterClear;timing=$timing;result=$result;fakeRpc=$true;relayDisabledBeforeStart=$true}|ConvertTo-Json -Depth 6|Set-Content (Join-Path $root 'verification.json') -Encoding UTF8
    Write-Host "Presence $Scenario PASS; reconnect=$($result.reconnected)"
}finally{
    foreach($n in $envNames){if($saved.ContainsKey($n)){[Environment]::SetEnvironmentVariable($n,$saved[$n],'Process')}}
    if($null -ne $process -and -not $process.HasExited){& $appFull /exit|Out-Null;if(-not $process.WaitForExit(10000)){Stop-Process -Id $process.Id -Force;throw 'Candidate required forced exit'}}
    $commands|ConvertTo-Json -Depth 5|Set-Content (Join-Path $root 'commands.json') -Encoding UTF8
    $job|Format-List State,JobStateInfo|Out-File (Join-Path $root 'server-state.log')
    $ErrorActionPreference='Continue'
    Receive-Job $job -ErrorAction Continue 2>&1|Out-File (Join-Path $root 'server-output.log')
    Remove-Job $job -Force -ErrorAction SilentlyContinue
}
