Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo 'out/tests/spectrum-resource-retry'
& python -B "$repo/tests/spectrum_retry_oracle.py" --output $output
if ($LASTEXITCODE) {throw 'Spectrum retry fixture extraction failed.'}
& "$PSScriptRoot/import-v142-environment.ps1" | Out-Null
Push-Location $output
try {
    & cl.exe /nologo /std:c++20 /O2 /MT /EHsc /utf-8 oracle.cpp /Fe:spectrum-retry-tests.exe
    if ($LASTEXITCODE) {throw 'Spectrum retry fixture did not compile.'}
    & ./spectrum-retry-tests.exe
    if ($LASTEXITCODE) {throw 'Spectrum resource retry regression.'}
} finally {Pop-Location}
