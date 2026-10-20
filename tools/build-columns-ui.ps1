param([string]$VsRoot='', [ValidateSet('v143')][string]$Toolset='v143',
    [ValidatePattern('^14\.44(\.\d+)*$')][string]$CompilerVersion='14.44',
    [string]$WindowsSdk='10.0.26100.0')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
& python -B (Join-Path $PSScriptRoot 'verify-build-inputs.py')
if ($LASTEXITCODE) {throw 'Pinned build input verification failed.'}
if ($repo -match '[\s"&%\^!()]') {throw 'The upstream Columns UI build hooks require a simple path without spaces, quotes or command-shell metacharacters.'}
if (!(Get-Command py -ErrorAction SilentlyContinue)) {throw 'The Python launcher (py) is required by upstream Columns UI build hooks.'}
& py -B -c 'import sys,tomllib; sys.exit(0 if sys.version_info >= (3,12) else 1)'
if ($LASTEXITCODE) {throw 'Columns UI requires the default py launcher to select Python 3.12+.'}
$lock=Get-Content (Join-Path $repo 'third_party/columns-ui/upstream.lock.json') -Raw | ConvertFrom-Json
$source=Join-Path $repo 'out/dependencies/columns-ui'
function Invoke-Git([string[]]$Arguments) { & git @Arguments; if($LASTEXITCODE -ne 0) {throw 'Columns UI source operation failed.'} }
if (!(Test-Path (Join-Path $source '.git'))) {Invoke-Git @('clone','--branch','v3.6.0','--depth','1',$lock.url,$source)}
if ((& git -C $source rev-parse HEAD).Trim() -ne $lock.commit) {throw 'Columns UI source commit mismatch.'}
Invoke-Git @('-C',$source,'submodule','update','--init','--recursive','--jobs','1')
$patch=Join-Path $repo 'third_party/columns-ui/0001-goden-pmv2-window-context.patch'
& git -C $source apply --reverse --check $patch 2>$null
if ($LASTEXITCODE -ne 0) {Invoke-Git @('-C',$source,'apply','--check',$patch);Invoke-Git @('-C',$source,'apply',$patch)}
if (!$VsRoot) {
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $VsRoot=(& $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
}
$vcvars=Join-Path $VsRoot 'VC/Auxiliary/Build/vcvars64.bat'
$vcpkg=Join-Path $VsRoot 'VC/vcpkg/scripts/buildsystems/msbuild'
if (!(Test-Path $vcvars) -or !(Test-Path "$vcpkg/vcpkg.props")) {throw 'VS2022 C++/vcpkg toolchain is required; pass -VsRoot explicitly.'}
$toolchain=Get-ChildItem -LiteralPath (Join-Path $VsRoot 'VC/Tools/MSVC') -Directory |
    Where-Object {($_.Name -eq $CompilerVersion -or $_.Name.StartsWith($CompilerVersion+'.')) -and
        (Test-Path (Join-Path $_.FullName 'bin/Hostx64/x64/cl.exe'))} |
    Sort-Object {[Version]$_.Name} -Descending | Select-Object -First 1
if (!$toolchain) {throw "Pinned Columns UI compiler $CompilerVersion is missing."}
$selectedVersion=$toolchain.Name
$environmentLines=& cmd.exe /d /s /c "`"$vcvars`" $WindowsSdk -vcvars_ver=$selectedVersion >nul && set"
if ($LASTEXITCODE) {throw 'Columns UI developer environment failed.'}
$environmentLines | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}
}
$env:VCPKG_MAX_CONCURRENCY='1';$env:CMAKE_BUILD_PARALLEL_LEVEL='1'
$solution=Join-Path $source 'vc18/columns_ui-public.sln'
$msbuild=Join-Path $VsRoot 'MSBuild/Current/Bin/amd64/MSBuild.exe'
# Materialize packages before snapshotting the effective source trees.
& $msbuild (Join-Path $source 'foo_ui_columns/foo_ui_columns.vcxproj') /t:VcpkgInstallManifestDependencies /m:1 /nr:false /p:Configuration=Release /p:Platform=x64 "/p:PlatformToolset=$Toolset" "/p:VCToolsVersion=$selectedVersion" "/p:WindowsTargetPlatformVersion=$WindowsSdk" /p:CL_MPCount=1 /p:BuildInParallel=false /p:VcpkgEnableManifest=true /p:VcpkgUseStatic=true /p:VcpkgTriplet=x64-windows-static "/p:ForceImportAfterCppProps=$vcpkg/vcpkg.props" "/p:ForceImportAfterCppTargets=$vcpkg/vcpkg.targets" /v:minimal /nologo
if ($LASTEXITCODE) {throw 'Columns UI dependency materialization failed before its source snapshot.'}
$sourceFingerprint=& python -B (Join-Path $PSScriptRoot 'public_release.py') dependency-fingerprint columns-ui
if ($LASTEXITCODE) {throw 'Columns UI pre-build source snapshot failed.'}
& $msbuild $solution /t:Build /m:1 /nr:false /p:Configuration=Release /p:Platform=x64 "/p:PlatformToolset=$Toolset" "/p:VCToolsVersion=$selectedVersion" "/p:WindowsTargetPlatformVersion=$WindowsSdk" /p:CL_MPCount=1 /p:BuildInParallel=false /p:UseMultiToolTask=false /p:VcpkgEnableManifest=true /p:VcpkgUseStatic=true /p:VcpkgTriplet=x64-windows-static "/p:ForceImportAfterCppProps=$vcpkg/vcpkg.props" "/p:ForceImportAfterCppTargets=$vcpkg/vcpkg.targets" /v:minimal /nologo
if ($LASTEXITCODE -ne 0) {throw 'Columns UI build failed.'}
$dll=Join-Path $source "vc18/release-x64-$Toolset/foo_ui_columns.dll"
New-Item -ItemType Directory -Force (Join-Path $repo 'out/Release') | Out-Null
Copy-Item $dll (Join-Path $repo 'out/Release/foo_ui_columns.dll')
@{sourceTreeSha256=$sourceFingerprint;upstreamCommit=$lock.commit;toolset=$Toolset;compilerVersion=$selectedVersion;windowsSdk=$WindowsSdk;sha256=(Get-FileHash $dll).Hash;patchSha256=(Get-FileHash $patch).Hash;submodules=@(& git -C $source submodule status --recursive)} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $repo 'out/Release/columns-ui-build.json') -Encoding utf8
& python (Join-Path $PSScriptRoot 'public_release.py') seal-dependency columns-ui
if ($LASTEXITCODE) {throw 'Columns UI source receipt failed.'}
