param([switch]$InstallMissingVisualStudioComponents)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if ($repo -match '[\s"&%\^!()]') {throw 'Use a simple checkout path without spaces, quotes or command-shell metacharacters, for example C:\src\GODENplayer.'}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path $vswhere)) {throw 'Install Visual Studio 2022 C++ Build Tools first.'}
$vs=& $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) {throw 'A Visual Studio C++ installation is required.'}
if ($InstallMissingVisualStudioComponents) {
    $installer=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vs_installer.exe'
    $arguments=@('modify','--installPath',('"'+$vs+'"'),'--quiet','--norestart',
        '--add','Microsoft.VisualStudio.Component.VC.14.29.16.11.x86.x64',
        '--add','Microsoft.VisualStudio.Component.VC.14.29.16.11.ATL',
        '--add','Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64',
        '--add','Microsoft.VisualStudio.Component.VC.CMake.Project',
        '--add','Microsoft.VisualStudio.Component.Vcpkg',
        '--add','Microsoft.VisualStudio.Component.Windows10SDK.19041',
        '--add','Microsoft.VisualStudio.Component.Windows11SDK.26100')
    $process=Start-Process $installer -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -notin @(0,3010)) {throw "Visual Studio preparation failed: $($process.ExitCode)"}
    if ($process.ExitCode -eq 3010) {throw 'Visual Studio preparation requires a restart before building.'}
}
foreach ($tool in 'git','python','py','node','npm','7z') {
    if (!(Get-Command $tool -ErrorAction SilentlyContinue)) {throw "Required build tool missing: $tool"}
}
& py -B -c 'import sys,tomllib; sys.exit(0 if sys.version_info >= (3,12) else 1)'
if ($LASTEXITCODE) {throw 'Columns UI requires the default py launcher to select Python 3.12+.'}
$nativeMsbuild=& "$PSScriptRoot/resolve-msbuild.ps1"
$nativeRoot=$nativeMsbuild.Substring(0,$nativeMsbuild.IndexOf('\MSBuild\',[StringComparison]::OrdinalIgnoreCase))
$nativeToolset=Get-ChildItem -LiteralPath (Join-Path $nativeRoot 'VC/Tools/MSVC') -Directory |
    Where-Object {$_.Name.StartsWith('14.29.')} | Sort-Object Name -Descending | Select-Object -First 1
if (!$nativeToolset) {throw 'MSVC v142 14.29 is required.'}
$atlRoot=Join-Path $nativeToolset.FullName 'atlmfc'
if ($env:GODEN_ATL_ROOT) {$atlRoot=[IO.Path]::GetFullPath($env:GODEN_ATL_ROOT)}
if (!(Test-Path (Join-Path $atlRoot 'include/atlbase.h')) -or !(Test-Path (Join-Path $atlRoot 'lib/x64/atls.lib'))) {
    throw 'MSVC 14.29 ATL is required; install it or explicitly set GODEN_ATL_ROOT to its licensed extraction.'
}
if (!(Test-Path (Join-Path $vs 'VC/Tools/MSVC/14.44.*'))) {throw 'MSVC v143 14.44 is required.'}
foreach ($relative in 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe',
        'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe',
        'VC/vcpkg/scripts/buildsystems/msbuild/vcpkg.props') {
    if (!(Test-Path (Join-Path $vs $relative))) {throw "Selected VS2022 is missing: $relative"}
}
& "$PSScriptRoot/resolve-native-build-tools.ps1" | Out-Null
$kits=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10'
foreach ($sdk in '10.0.19041.0','10.0.26100.0') {
    if (!(Test-Path (Join-Path $kits "bin/$sdk/x64/fxc.exe"))) {throw "Windows SDK missing: $sdk"}
}
& "$PSScriptRoot/prepare-public-dependencies.ps1"
Write-Host 'Public native build prerequisites checked; no player/profile was changed.'
