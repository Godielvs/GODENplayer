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
if ($LASTEXITCODE) { throw 'Pinned build input verification failed before libsonare compilation.' }
$source = Join-Path $repo 'vendor\libsonare'
$eigen = Join-Path $repo 'vendor\eigen'
$build = Join-Path $repo "out\third_party\libsonare\$Configuration"

foreach ($required in @(
    (Join-Path $source 'CMakeLists.txt'),
    (Join-Path $source 'LICENSE'),
    (Join-Path $source 'NOTICE'),
    (Join-Path $eigen 'COPYING.MPL2')
)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Pinned analysis dependency is incomplete: $required"
    }
}

& "$PSScriptRoot/import-v142-environment.ps1" | Out-Null
$buildTools=& "$PSScriptRoot/resolve-native-build-tools.ps1"
$cmake=$buildTools.CMake
$ninja=$buildTools.Ninja
$env:VSLANG='1033'

$runtime = if ($Configuration -eq 'Release') { 'MultiThreaded' } else { 'MultiThreadedDebug' }
$configureArguments = @(
    '-S', $source,
    '-B', $build,
    '-G', 'Ninja',
    "-DCMAKE_MAKE_PROGRAM:FILEPATH=$ninja",
    "-DCMAKE_BUILD_TYPE=$Configuration",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=$runtime",
    '-DCMAKE_CXX_FLAGS=/EHsc /GR /wd4127 /wd4456 /wd4706',
    "-DFETCHCONTENT_SOURCE_DIR_EIGEN3=$eigen",
    '-DCMAKE_DISABLE_FIND_PACKAGE_Eigen3=ON',
    '-DBUILD_TESTING=OFF',
    '-DBUILD_CLI=OFF',
    '-DBUILD_SHARED=OFF',
    '-DBUILD_BENCH=OFF',
    '-DBUILD_MASTERING=ON',
    '-DBUILD_MIXING=OFF',
    '-DBUILD_GRAPH=OFF',
    '-DBUILD_FX=ON',
    '-DBUILD_ACOUSTIC_SIM=ON',
    '-DBUILD_VOICE_CHANGER=OFF',
    '-DBUILD_ARRANGEMENT=OFF',
    '-DBUILD_ASSIST=OFF',
    '-DSONARE_WITH_FFMPEG=OFF'
)

& $cmake @configureArguments
if ($LASTEXITCODE -ne 0) {
    throw "libsonare configure failed with exit code $LASTEXITCODE."
}
$buildLog = Join-Path $build 'nekorox-libsonare-build.log'
& $cmake --build $build --config $Configuration --target sonare_mastering --parallel $Workers *> $buildLog
$buildExitCode = $LASTEXITCODE
if ($buildExitCode -ne 0) {
    Get-Content -LiteralPath $buildLog | Select-Object -Last 160 | Write-Host
    throw "libsonare build failed with exit code $buildExitCode."
}

$coreLibrary = Join-Path $build 'lib\sonare_core.lib'
$realtimeLibrary = Join-Path $build 'lib\sonare_rt.lib'
$fftLibrary = Join-Path $build 'lib\kissfft.lib'
$masteringLibrary = Join-Path $build 'lib\sonare_mastering.lib'
$fxLibrary = Join-Path $build 'lib\sonare_fx.lib'
$acousticLibrary = Join-Path $build 'lib\sonare_acoustic.lib'
if (-not (Test-Path -LiteralPath $coreLibrary) -or
    -not (Test-Path -LiteralPath $realtimeLibrary) -or
    -not (Test-Path -LiteralPath $fftLibrary) -or
    -not (Test-Path -LiteralPath $masteringLibrary) -or
    -not (Test-Path -LiteralPath $fxLibrary) -or
    -not (Test-Path -LiteralPath $acousticLibrary)) {
    throw 'libsonare build completed without the expected static archives.'
}

$manifest = [ordered]@{
    schema = 'nekorox.analysis-dependency-build.v1'
    configuration = $Configuration
    runtime = $runtime
    libsonare = [ordered]@{
        version = '1.5.2'
        commit = '30931acd0d16e5d890b728548dd74675ec13758c'
        target = 'sonare_mastering'
        features = @('mastering', 'fx', 'acoustic-sim')
        libraries = @(
            $coreLibrary,
            $realtimeLibrary,
            $masteringLibrary,
            $fxLibrary,
            $acousticLibrary,
            $fftLibrary
        )
    }
    eigen = [ordered]@{
        version = '3.4.0'
        commit = '3147391d946bb4b6c68edd901f2add6ac1f31f8c'
    }
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $build 'nekorox-build-manifest.json') -Encoding UTF8
