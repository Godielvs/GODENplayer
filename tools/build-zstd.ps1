param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$PythonExecutable = 'python.exe',
    [ValidateRange(1, 1)]
    [int]$Workers = 1
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
& $PythonExecutable -B "$PSScriptRoot/verify-build-inputs.py"
if ($LASTEXITCODE) { throw 'Pinned build input verification failed before zstd compilation.' }
$source = Join-Path $repo 'vendor\zstd\build\cmake'
$vendorRoot = Join-Path $repo 'vendor\zstd'
$build = Join-Path $repo "out\third_party\zstd\$Configuration"
foreach ($required in @(
    (Join-Path $source 'CMakeLists.txt'),
    (Join-Path $vendorRoot 'LICENSE')
)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Pinned zstd dependency is incomplete: $required"
    }
}

& "$PSScriptRoot/import-v142-environment.ps1" | Out-Null
$buildTools=& "$PSScriptRoot/resolve-native-build-tools.ps1"
$cmake=$buildTools.CMake
$ninja=$buildTools.Ninja
$env:VSLANG='1033'

$runtime = if ($Configuration -eq 'Release') { 'MultiThreaded' } else { 'MultiThreadedDebug' }
& $cmake `
    -S $source `
    -B $build `
    -G Ninja `
    "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninja" `
    "-DCMAKE_BUILD_TYPE=$Configuration" `
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=$runtime" `
    -DCMAKE_POLICY_DEFAULT_CMP0091=NEW `
    -DZSTD_BUILD_PROGRAMS=OFF `
    -DZSTD_BUILD_TESTS=OFF `
    -DZSTD_BUILD_CONTRIB=OFF `
    -DZSTD_BUILD_SHARED=OFF `
    -DZSTD_BUILD_STATIC=ON `
    -DZSTD_BUILD_DICTBUILDER=OFF `
    -DZSTD_LEGACY_SUPPORT=OFF `
    -DZSTD_MULTITHREAD_SUPPORT=OFF
if ($LASTEXITCODE -ne 0) {
    throw "zstd configure failed with exit code $LASTEXITCODE."
}

$buildLog = Join-Path $build 'nekorox-zstd-build.log'
& $cmake --build $build --config $Configuration --target libzstd_static --parallel $Workers *> $buildLog
$buildExitCode = $LASTEXITCODE
if ($buildExitCode -ne 0) {
    Get-Content -LiteralPath $buildLog | Select-Object -Last 160 | Write-Host
    throw "zstd build failed with exit code $buildExitCode."
}

$library = Join-Path $build 'lib\zstd_static.lib'
if (-not (Test-Path -LiteralPath $library)) {
    throw 'zstd build completed without zstd_static.lib.'
}

[ordered]@{
    schema = 'nekorox.compression-dependency-build.v1'
    configuration = $Configuration
    runtime = $runtime
    zstd = [ordered]@{
        version = '1.5.7'
        commit = 'f8745da6ff1ad1e7bab384bd1f9d742439278e99'
        target = 'libzstd_static'
        library = $library
    }
} | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $build 'nekorox-build-manifest.json') -Encoding UTF8
