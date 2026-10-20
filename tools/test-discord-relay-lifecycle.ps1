param([string]$ManifestPath='')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if(!$ManifestPath){$ManifestPath=Join-Path $repo 'artifacts\discord-relay-lifecycle.json'}
$inputs=@('src/component/discord_relay_client.cpp','src/component/discord_relay_client.h',
    'src/component/discord_http_io.h','src/component/qa_hooks.h','src/component/presence_cover_policy.h',
    'src/component/presence_deployment.h','tests/discord_relay_lifecycle_tests.cpp','tests/nekorox_relay_tests.vcxproj')
$hashes=@{}
foreach($inputPath in $inputs){$hashes[$inputPath]=(Get-FileHash -LiteralPath (Join-Path $repo $inputPath)).Hash}
$msbuild=& "$PSScriptRoot\resolve-msbuild.ps1"
& $msbuild "$repo\tests\nekorox_relay_tests.vcxproj" /m:1 /nodeReuse:false /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v142 /p:WindowsTargetPlatformVersion=10.0.19041.0 /p:NekoBuildWorkers=1 /p:CL_MPCount=1 /p:MultiProcMaxCount=1 /v:minimal /nologo
if($LASTEXITCODE){throw 'Relay lifecycle fixture build failed.'}
$exe=Join-Path $repo 'out\Release\nekorox_relay_tests.exe'
$result=& $exe 2>&1
$exitCode=$LASTEXITCODE
$result | ForEach-Object {Write-Host $_}
if($exitCode){throw 'Relay lifecycle fixture failed.'}
foreach($inputPath in $inputs){
    if((Get-FileHash -LiteralPath (Join-Path $repo $inputPath)).Hash -ne $hashes[$inputPath]){
        throw "Source changed during qualification: $inputPath"
    }
}
$binary=[Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exe))
if($binary -match '(?i)winhttp\.dll'){throw 'Fake transport unexpectedly imports WinHTTP.'}
$manifest=[ordered]@{
    schema='goden.discord-relay-lifecycle.v1';createdUtc=[DateTime]::UtcNow.ToString('o');passed=$true
    transport='Fake WinHTTP; production client and shared async handle implementation; no network'
    sources=$hashes;exe=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash;results=@($result|ForEach-Object{$_.ToString()})
}
[IO.Directory]::CreateDirectory((Split-Path -Parent ([IO.Path]::GetFullPath($ManifestPath))))|Out-Null
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ManifestPath -Encoding UTF8
