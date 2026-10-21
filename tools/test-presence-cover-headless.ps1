param([string]$ManifestPath='')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if (!$ManifestPath) {$ManifestPath=Join-Path $repo 'artifacts/presence-cover-headless.json'}
$generated=Join-Path $repo 'out/cover-fixture'
New-Item -ItemType Directory -Force $generated | Out-Null
$inputs=@('src/component/discord_cover_service.cpp','src/component/discord_cover_service.h',
    'src/component/discord_relay_client.cpp','src/component/discord_relay_client.h',
    'src/component/discord_http_io.h','src/component/presence_cover_policy.h',
    'src/component/presence_public_hosts.h','vendor/nlohmann-json/nlohmann/json.hpp',
    'src/component/qa_hooks.h','src/component/presence_deployment.h','tests/discord_relay_lifecycle_tests.cpp',
    'tests/discord_cover_lifecycle_tests.cpp','tests/nekorox_cover_tests.vcxproj',
    'tools/test-presence-cover-headless.ps1')
$hashes=@{}
foreach ($relative in $inputs) {$hashes[$relative]=(Get-FileHash (Join-Path $repo $relative)).Hash}
# Substitute host-only includes and access labels; preserve every provider
# method body, WIC encoder, cache parser, cancellation path and HTTP operation.
$header=Get-Content (Join-Path $repo 'src/component/discord_cover_service.h') -Raw
foreach ($include in '#include "art_cache.h"','#include "discord_relay_client.h"') {
    if (!$header.Contains($include)) {throw "Provider fixture header seam changed: $include"}
    $header=$header.Replace($include,'')
}
$header=$header.Replace('private:','public:')
$header | Set-Content (Join-Path $generated 'discord_cover_service.fixture.h') -Encoding utf8
$source=Get-Content (Join-Path $repo 'src/component/discord_cover_service.cpp') -Raw
foreach ($include in '#include "rollout_flags.h"','#include "text_util.h"') {
    if (!$source.Contains($include)) {throw "Provider fixture host seam changed: $include"}
    $source=$source.Replace($include,'')
}
$source=$source.Replace('#include "discord_cover_service.h"','#include "discord_cover_service.fixture.h"')
$source | Set-Content (Join-Path $generated 'discord_cover_service.fixture.cpp') -Encoding utf8
$msbuild=& "$PSScriptRoot/resolve-msbuild.ps1"
& $msbuild "$repo/tests/nekorox_cover_tests.vcxproj" /m:1 /nodeReuse:false /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v142 /p:WindowsTargetPlatformVersion=10.0.19041.0 /p:NekoBuildWorkers=1 /p:CL_MPCount=1 /v:minimal /nologo
if ($LASTEXITCODE) {throw 'Cover provider fixture build failed.'}
$exe=Join-Path $repo 'out/Release/nekorox_cover_tests.exe'
$fixtureRoot=Join-Path $repo ('artifacts/cover-provider-'+[Guid]::NewGuid().ToString('N'))
$result=& $exe $fixtureRoot 2>&1
$exitCode=$LASTEXITCODE
$result | ForEach-Object {Write-Host $_}
if ($exitCode) {throw 'Cover provider fixture failed.'}
foreach ($relative in $inputs) {
    if ((Get-FileHash (Join-Path $repo $relative)).Hash -ne $hashes[$relative]) {throw "Source changed during test: $relative"}
}
if ([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($exe)) -match '(?i)winhttp\.dll') {
    throw 'Provider fixture unexpectedly imports a real network API.'
}
New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($ManifestPath))) | Out-Null
[ordered]@{schema='goden.cover-provider-lifecycle.v1';passed=$true;createdUtc=[DateTime]::UtcNow.ToString('o');
    transport='Fake WinHTTP, unchanged production provider method bodies and real WIC; host artwork/profile seams stubbed';
    sources=$hashes;sha256=(Get-FileHash $exe).Hash;results=@($result|ForEach-Object {$_.ToString()});
    fullPlayerTested=$false;discordClientTested=$false} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ManifestPath -Encoding utf8
