Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo 'out/tests/analysis-revision'
& python -B "$repo/tests/analysis_revision_oracle.py" --output $output
if ($LASTEXITCODE) {throw 'Analysis revision fixture extraction failed.'}
& "$PSScriptRoot/import-v142-environment.ps1" | Out-Null
Push-Location $output
try {
    & cl.exe /nologo /std:c++20 /O2 /MT /EHsc /utf-8 /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I "$repo/src/core/include" oracle.cpp "$repo/src/core/src/track_identity.cpp" /Fe:analysis-revision-tests.exe /link Bcrypt.lib
    if ($LASTEXITCODE) {throw 'Analysis revision fixture did not compile.'}
    & ./analysis-revision-tests.exe
    if ($LASTEXITCODE) {throw 'Analysis revision admission regression.'}
} finally {Pop-Location}
