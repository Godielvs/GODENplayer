param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$UseWarp
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$r6Msbuild = & "$PSScriptRoot\resolve-msbuild.ps1"
& $r6Msbuild "$repo\tests\r6_d2d_effect_probe.vcxproj" `
    /m:1 /nodeReuse:false /t:Build `
    /p:Configuration=$Configuration /p:Platform=x64 `
    /p:PlatformToolset=v142 /p:WindowsTargetPlatformVersion=10.0.19041.0 `
    /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw 'R6 D2D effect probe build failed.' }
$probeArguments=@()
if ($UseWarp) {$probeArguments+='--warp'}
& "$repo\out\$Configuration\r6_d2d_effect_probe.exe" @probeArguments
if ($LASTEXITCODE -ne 0) { throw 'R6 D2D effect probe failed.' }
