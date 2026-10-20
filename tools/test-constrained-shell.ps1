Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo 'out/tests/constrained-shell'
& python -B "$repo/tests/constrained_shell_oracle.py" --output $output
if ($LASTEXITCODE) {throw 'Shell placement extraction failed.'}
& "$PSScriptRoot/import-v142-environment.ps1" | Out-Null
Push-Location $output
try {
    & cl.exe /nologo /std:c++20 /O2 /MT /EHsc /utf-8 /DNOMINMAX /DUNICODE /D_UNICODE /I "$repo/src/component" /I "$repo/src/core/include" shell-oracle.cpp /Fe:shell-oracle.exe /link user32.lib
    if ($LASTEXITCODE) {throw 'Shell placement fixture did not compile.'}
    & ./shell-oracle.exe
    if ($LASTEXITCODE) {throw 'Shell constrained-layout integration failed.'}
} finally {Pop-Location}
