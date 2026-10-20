param([string]$Configuration='Release')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo 'out/tests/master-dsp-engine'
New-Item -ItemType Directory -Force $output | Out-Null
& python -B "$repo/tests/master_dsp_engine_headless.py" --output "$output/engine.cpp"
if ($LASTEXITCODE) {throw 'DSP fixture extraction failed.'}
& "$PSScriptRoot/import-v142-environment.ps1" | Out-Null
$libraries=@('sonare_mastering','sonare_fx','sonare_acoustic','sonare_rt','sonare_core','kissfft') | ForEach-Object {Join-Path $repo "out/third_party/libsonare/$Configuration/lib/$_.lib"}
Push-Location $output
try {
    & cl.exe /nologo /std:c++20 /O2 /MT /EHsc /utf-8 /DNDEBUG /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I "$repo/src/component" /I "$repo/src/core/include" /I "$repo/vendor/libsonare/src" /I "$repo/vendor/libsonare/include" /I "$repo/vendor/libsonare/third_party/r8brain" "$output/engine.cpp" "$repo/vendor/libsonare/src/mixing/meter.cpp" /Fe:engine-tests.exe /link @libraries
    if ($LASTEXITCODE) {throw 'Actual DSP engine fixture did not compile.'}
    & "$output/engine-tests.exe"
    if ($LASTEXITCODE) {throw 'Actual DSP engine regression.'}
} finally {Pop-Location}
