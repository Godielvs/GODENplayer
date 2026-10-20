Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$output=Join-Path $repo 'out/tests/system-ui-preferences'
if (Get-Process cl, link, MSBuild -ErrorAction SilentlyContinue) {
    throw 'Finish existing builds before compiling UI preference tests.'
}
& python -B "$PSScriptRoot/system-ui-preferences-oracle.py" --output $output
if ($LASTEXITCODE) {throw 'UI preference extraction failed.'}
& "$PSScriptRoot/import-v142-environment.ps1" | Out-Null
Push-Location $output
try {
    & cl.exe /nologo /std:c++20 /O2 /MT /EHsc /utf-8 /DNOMINMAX /DUNICODE /D_UNICODE /I "$repo/src/component" /I $output "$repo/tests/system_ui_preferences_tests.cpp" /Fe:system-ui-preferences.exe /link user32.lib dwrite.lib d2d1.lib windowscodecs.lib ole32.lib
    if ($LASTEXITCODE) {throw 'UI preference fixture did not compile.'}
    & ./system-ui-preferences.exe
    if ($LASTEXITCODE) {throw 'UI preference or raster regression.'}
} finally {Pop-Location}
