param(
    [ValidateRange(1, 120)][int]$SoakSeconds = 60
)

# Requires Windows and a working hardware D3D11 adapter. Creates real hardware
# and WARP devices, but does not disable drivers, show the player or alter its
# profile. Native display messages are sent only to the probe's own window.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo ('artifacts\hub-worker-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
if (Get-Process cl, link, MSBuild -ErrorAction SilentlyContinue) {
    throw 'Finish existing builds before running graphics qualification.'
}
$msbuild = & (Join-Path $PSScriptRoot 'resolve-msbuild.ps1')
$marker = $msbuild.IndexOf('\MSBuild\', [StringComparison]::OrdinalIgnoreCase)
$vcvars = Join-Path $msbuild.Substring(0, $marker) 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw 'vcvars64.bat unavailable' }
foreach ($path in @($repo, $output, $vcvars)) {
    if ($path -match '[%!"&|<>^\r\n]') { throw 'Path contains unsupported cmd characters.' }
}
New-Item -ItemType Directory -Path $output | Out-Null
$probes = @(
    @{ Name = 'render_device_hub_recovery_probe'; Extra = '' },
    @{ Name = 'render_device_hub_environment_probe'; Extra = 'src\component\display_environment.cpp' }
)
Push-Location $repo
try {
    foreach ($probe in $probes) {
        $name = $probe.Name
        $dir = Join-Path $output $name
        New-Item -ItemType Directory -Path $dir | Out-Null
        $commandFile = Join-Path $dir 'build.cmd'
        @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /EHsc /MD /O2 /DNOMINMAX /DUNICODE /D_UNICODE /DNEKOROX_RENDER_HUB_TEST /Isrc\component /Isrc\core\include tests\$name.cpp src\component\render_device_hub.cpp $($probe.Extra) /Fo"$dir\\" /Fe"$dir\$name.exe" /link d2d1.lib d3d11.lib dxgi.lib dwrite.lib ole32.lib user32.lib dwmapi.lib wtsapi32.lib
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $commandFile -Encoding Default
        & $env:ComSpec /c $commandFile *> (Join-Path $dir 'build.log')
        if ($LASTEXITCODE -ne 0) { throw "Build failed; see $dir\build.log" }
    }
    # Sequential builds finish before either timed test starts.
    foreach ($probe in $probes) {
        $name = $probe.Name
        $dir = Join-Path $output $name
        $arguments = @()
        if ($name -eq 'render_device_hub_recovery_probe') { $arguments = @($SoakSeconds) }
        & (Join-Path $dir "$name.exe") @arguments *> (Join-Path $dir 'result.log')
        if ($LASTEXITCODE -ne 0) { throw "Probe failed; see $dir\result.log" }
        Get-Content -LiteralPath (Join-Path $dir 'result.log')
    }
    Write-Host "Graphics recovery qualification PASS: $output"
} finally {
    Pop-Location
}
