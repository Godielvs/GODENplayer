param([string]$FixtureRoot='',[string]$OutputRoot='')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$msbuild=& "$PSScriptRoot\resolve-msbuild.ps1"
& $msbuild "$repo\tests\nekorox_opus_info_tests.vcxproj" /m:1 /nodeReuse:false /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v142 /p:WindowsTargetPlatformVersion=10.0.19041.0 /p:NekoBuildWorkers=1 /v:minimal /nologo
if($LASTEXITCODE){throw 'Opus metadata tests build failed.'}
$exe=Join-Path $repo 'out\Release\nekorox_opus_info_tests.exe'
if($FixtureRoot){
    if(!$OutputRoot){throw 'Specify an artifacts output directory.'}
    $full=[IO.Path]::GetFullPath($OutputRoot)
    if(!$full.StartsWith((Join-Path $repo 'artifacts')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Fixture output must stay within artifacts.'}
    & $exe $FixtureRoot $full
}else{& $exe}
if($LASTEXITCODE){throw 'Opus metadata tests failed.'}
