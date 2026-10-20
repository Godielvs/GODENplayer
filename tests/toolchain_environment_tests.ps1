Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$import=Join-Path $repo 'tools/import-v142-environment.ps1'
$first=& $import
$names=@('PATH','INCLUDE','LIB','LIBPATH','VCToolsVersion',
    'VCToolsInstallDir','WindowsSDKVersion','WindowsSdkDir')
$before=@{}
foreach($name in $names) {$before[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
$compiler=(Get-Command cl.exe -ErrorAction Stop).Source
for($attempt=0;$attempt -lt 10;++$attempt) {
    if ((& $import) -cne $first) {throw 'Selected toolchain changed during repeat initialization.'}
    foreach($name in $names) {
        if ([Environment]::GetEnvironmentVariable($name,'Process') -cne $before[$name]) {
            throw "Repeated initialization expanded or changed $name."
        }
    }
    if ((Get-Command cl.exe -ErrorAction Stop).Source -cne $compiler) {
        throw 'Repeated initialization selected a different compiler.'
    }
}
Write-Host 'Pinned toolchain environment: PASS (10 repeat initializations, no growth)'
