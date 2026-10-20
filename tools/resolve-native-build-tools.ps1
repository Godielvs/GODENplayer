Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path $vswhere)) {throw 'Visual Studio 2022 CMake/Ninja tools are required.'}
$toolsRoot=& $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
if (!$toolsRoot) {throw 'Install the Visual Studio 2022 C++ CMake tools component.'}
$cmake=Join-Path $toolsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$ninja=Join-Path $toolsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
if (!(Test-Path $cmake) -or !(Test-Path $ninja)) {throw 'Selected VS2022 CMake/Ninja component is incomplete.'}
$versionText=& $cmake --version
if ($LASTEXITCODE -or $versionText[0] -notmatch 'cmake version (\d+\.\d+\.\d+)') {throw 'Could not query CMake version.'}
$cmakeVersion=$Matches[1]
if ([Version]$cmakeVersion -lt [Version]'3.27.0') {throw 'CMake 3.27+ is required.'}
$ninjaVersion=(& $ninja --version | Select-Object -First 1).Trim()
if ($LASTEXITCODE -or $ninjaVersion -notmatch '^\d+\.\d+\.\d+$' -or [Version]$ninjaVersion -lt [Version]'1.11.0') {throw 'Ninja 1.11+ is required.'}
[PSCustomObject]@{CMake=$cmake;Ninja=$ninja;CMakeVersion=$cmakeVersion;NinjaVersion=$ninjaVersion}
