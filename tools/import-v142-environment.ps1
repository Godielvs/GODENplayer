Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$msbuild=& "$PSScriptRoot/resolve-msbuild.ps1"
$marker=$msbuild.IndexOf('\MSBuild\',[StringComparison]::OrdinalIgnoreCase)
if ($marker -lt 0) {throw 'Could not resolve the selected Visual Studio installation.'}
$vsRoot=$msbuild.Substring(0,$marker)
$environmentStamp = {
    $fields = @($vsRoot) + @('PATH','INCLUDE','LIB','LIBPATH','VCToolsVersion',
        'VCToolsInstallDir','WindowsSDKVersion','WindowsSdkDir',
        'VSCMD_ARG_HOST_ARCH','VSCMD_ARG_TGT_ARCH' | ForEach-Object {
            [Environment]::GetEnvironmentVariable($_, 'Process')
        })
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { [Convert]::ToBase64String($hasher.ComputeHash(
        [Text.Encoding]::UTF8.GetBytes(($fields -join "`0")))) }
    finally { $hasher.Dispose() }
}
# VsDevCmd is not safely repeatable with an already-expanded environment in
# every supported installation. Reuse only an exact environment we validated
# in this PowerShell process, not an arbitrary inherited developer prompt.
$previous = Get-Variable -Name GodenV142EnvironmentStamp -Scope Global -ErrorAction SilentlyContinue
if ($null -ne $previous -and $previous.Value -ceq (& $environmentStamp)) {
    $vsRoot
    return
}
$command=Join-Path $vsRoot 'Common7/Tools/VsDevCmd.bat'
$environmentLines=& cmd.exe /d /s /c "`"$command`" -no_logo -arch=x64 -host_arch=x64 -vcvars_ver=14.29 -winsdk=10.0.19041.0 && set"
if ($LASTEXITCODE) {throw 'Could not initialize the pinned MSVC 14.29 environment.'}
$environmentLines | ForEach-Object {
    if($_ -match '^([^=]+)=(.*)$') {[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}
}
if (!$env:VCToolsVersion -or !$env:VCToolsVersion.StartsWith('14.29.')) {
    throw 'Developer environment did not select the pinned MSVC 14.29 toolset.'
}
if ($env:WindowsSDKVersion.TrimEnd('\') -ne '10.0.19041.0') {throw 'Developer environment did not select Windows SDK 10.0.19041.0.'}
$compiler=Get-Command cl.exe -ErrorAction Stop
$expectedCompiler=Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/cl.exe'
if (![IO.Path]::GetFullPath($compiler.Source).Equals([IO.Path]::GetFullPath($expectedCompiler),[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Inherited PATH resolved a different compiler than the selected MSVC 14.29 toolset.'
}
Set-Variable -Name GodenV142EnvironmentStamp -Scope Global -Value (& $environmentStamp)
$vsRoot
