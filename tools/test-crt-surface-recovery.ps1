param()

# Build the production component first to generate its five CRT shader headers.
# This probe uses real D3D11/DComp hardware + WARP and its own no-activate HWND;
# it never loads or changes the personal player/profile. One compiler at a time.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo ('artifacts\crt-recovery-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$shaders = Join-Path $repo 'out\obj\foo_nekorox\Release\crt-shaders'
if (Get-Process cl, link, MSBuild -ErrorAction SilentlyContinue) {
    throw 'Finish existing builds before running graphics qualification.'
}
foreach ($name in @('crt_fullscreen_vs.h', 'crt_beam_vs.h', 'crt_beam_ps.h', 'crt_decay_ps.h', 'crt_composite_ps.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $shaders $name))) {
        throw 'Build the Release component first to generate its CRT shaders.'
    }
}
$msbuild = & (Join-Path $PSScriptRoot 'resolve-msbuild.ps1')
$marker = $msbuild.IndexOf('\MSBuild\', [StringComparison]::OrdinalIgnoreCase)
$vcvars = Join-Path $msbuild.Substring(0, $marker) 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw 'vcvars64.bat unavailable' }
foreach ($path in @($repo, $output, $vcvars, $shaders)) {
    if ($path -match '[%!"&|<>^\r\n]') { throw 'Path contains unsupported cmd characters.' }
}
New-Item -ItemType Directory -Path $output | Out-Null
$commandFile = Join-Path $output 'build.cmd'
@"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /EHsc /MD /O2 /DNOMINMAX /DUNICODE /D_UNICODE /DNEKOROX_CRT_SURFACE_TEST /Isrc\component /Isrc\core\include /I"$shaders" tests\crt_surface_recovery_probe.cpp src\component\crt_xy_surface.cpp /Fo"$output\\" /Fe"$output\crt-recovery.exe" /link d3d11.lib dxgi.lib dcomp.lib ole32.lib user32.lib
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $commandFile -Encoding Default
Push-Location $repo
try {
    & $env:ComSpec /c $commandFile *> (Join-Path $output 'build.log')
    if ($LASTEXITCODE -ne 0) { throw "Build failed; see $output\build.log" }
    & (Join-Path $output 'crt-recovery.exe') *> (Join-Path $output 'result.log')
    $result = $LASTEXITCODE
    Get-Content -LiteralPath (Join-Path $output 'result.log')
    if ($result -ne 0) { throw "Probe failed ($result); see $output\result.log" }
    Write-Host "CRT recovery qualification PASS: $output"
} finally {
    Pop-Location
}
