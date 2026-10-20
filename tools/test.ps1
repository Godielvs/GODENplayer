param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$VerifyDeterminism,
    [switch]$SkipBuild,
    [switch]$UseWarp,
    [switch]$SkipPerformanceGates
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (!$SkipBuild) {
& "$PSScriptRoot\build.ps1" -Configuration $Configuration `
    -VerifyDeterminism:$VerifyDeterminism
}
$msbuild = & "$PSScriptRoot\resolve-msbuild.ps1"
& $msbuild "$repo\tests\master_dsp_libsonare_smoke.vcxproj" `
    /m:1 /nodeReuse:false /t:Build "/p:Configuration=$Configuration" `
    /p:Platform=x64 /p:PlatformToolset=v142 `
    /p:WindowsTargetPlatformVersion=10.0.19041.0 /v:minimal /nologo
if ($LASTEXITCODE -ne 0) {
    throw "GODENplayer Master DSP smoke-test build failed with exit code $LASTEXITCODE."
}
& $msbuild "$repo\tests\master_dsp_integration_tests.vcxproj" `
    /m:1 /nodeReuse:false /t:Build "/p:Configuration=$Configuration" `
    /p:Platform=x64 /p:PlatformToolset=v142 `
    /p:WindowsTargetPlatformVersion=10.0.19041.0 /v:minimal /nologo
if ($LASTEXITCODE -ne 0) {
    throw "GODENplayer Master DSP integration-test build failed with exit code $LASTEXITCODE."
}
& "$repo\out\$Configuration\nekorox_tests.exe"
if ($LASTEXITCODE -ne 0) {
    throw "NekoRoX tests failed with exit code $LASTEXITCODE."
}
& "$PSScriptRoot\test-r6-d2d-effect.ps1" -Configuration $Configuration -UseWarp:$UseWarp
& "$repo\out\$Configuration\master_dsp_libsonare_smoke.exe"
if ($LASTEXITCODE -ne 0) {
    throw "GODENplayer Master DSP catalog tests failed with exit code $LASTEXITCODE."
}
$dspArguments=@()
if ($SkipPerformanceGates) {$dspArguments+='--functional-only'}
& "$repo\out\$Configuration\master_dsp_integration_tests.exe" @dspArguments
if ($LASTEXITCODE -ne 0) {
    throw "GODENplayer Master DSP integration tests failed with exit code $LASTEXITCODE."
}
& npm --prefix "$repo\services\presence-relay" run check
if ($LASTEXITCODE -ne 0) {
    throw "GODENplayer Presence relay tests failed with exit code $LASTEXITCODE."
}
& "$PSScriptRoot\test-display-dpi-contract.ps1" `
    -Configuration $Configuration
& "$PSScriptRoot\test-refresh-presentation-contract.ps1" `
    -Configuration $Configuration
& "$PSScriptRoot\test-listen-spectrum-contract.ps1" `
    -Configuration $Configuration
& node "$PSScriptRoot\test-layout.js"
if ($LASTEXITCODE -ne 0) {
    throw "NekoRoX layout tests failed with exit code $LASTEXITCODE."
}
& "$repo\tests\corrective_r2_shell_integration.ps1" -RepoRoot $repo
& node "$PSScriptRoot\test-waveseek-layout.js" `
    "$repo\config\layout\nekorox-v3-waveseek-candidate.fcl"
if ($LASTEXITCODE -ne 0) {
    throw "NekoRoX WaveSeek layout tests failed with exit code $LASTEXITCODE."
}
& node "$PSScriptRoot\test-cohesive-bottom-layout.js" `
    "$repo\config\layout\nekorox-v3-cohesive-bottom.fcl"
if ($LASTEXITCODE -ne 0) {
    throw "NekoRoX cohesive bottom layout tests failed with exit code $LASTEXITCODE."
}
$sanitizerRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ('goden-shell-sanitizer-' + [Guid]::NewGuid().ToString('N'))
$sanitizerLayoutDirectory = Join-Path $sanitizerRoot 'nekorox\presets'
New-Item -ItemType Directory -Path $sanitizerLayoutDirectory | Out-Null
try {
    $sanitizerLayout = Join-Path $sanitizerLayoutDirectory 'layout.fcl'
    Copy-Item -LiteralPath `
        "$repo\config\layout\nekorox-v3-cohesive-bottom.fcl" `
        -Destination $sanitizerLayout
    & node "$PSScriptRoot\sanitize-friend-profile.js" $sanitizerRoot `
        (Join-Path $sanitizerRoot 'privacy.json') | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Shell-aware profile sanitizer failed with exit code $LASTEXITCODE."
    }
    & node "$PSScriptRoot\test-layout.js" $sanitizerLayout
    if ($LASTEXITCODE -ne 0) {
        throw "Sanitized shell layout is structurally invalid."
    }
    & node "$PSScriptRoot\test-cohesive-bottom-layout.js" $sanitizerLayout
    if ($LASTEXITCODE -ne 0) {
        throw "Sanitized cohesive shell layout is structurally invalid."
    }
}
finally {
    $resolvedSanitizerRoot = [IO.Path]::GetFullPath($sanitizerRoot)
    $temporaryPrefix = [IO.Path]::GetFullPath(
        [IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (($resolvedSanitizerRoot + '\').StartsWith(
            $temporaryPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $resolvedSanitizerRoot -Recurse -Force `
            -ErrorAction SilentlyContinue
    }
}
& node "$PSScriptRoot\test-spectrum-legacy-response.js"
if ($LASTEXITCODE -ne 0) {
    throw "NekoRoX legacy spectrum response tests failed with exit code $LASTEXITCODE."
}
& "$PSScriptRoot\test-analysis-stereo-v2.ps1" `
    -Configuration $Configuration
& node "$PSScriptRoot\test-openlyrics-config.js"
if ($LASTEXITCODE -ne 0) {
    throw "OpenLyrics local-only config tests failed with exit code $LASTEXITCODE."
}
& "$PSScriptRoot\test-profile-policy.ps1"
& "$repo\tests\r4_installer_contract_tests.ps1" -RepoRoot $repo
& "$PSScriptRoot\test-copy-lint.ps1"
& python -B "$repo\tests\public_release_tests.py"
if ($LASTEXITCODE) {throw 'Public archive/binary correspondence tests failed.'}
& python -B "$repo\tests\public_privacy_tests.py"
if ($LASTEXITCODE) {throw 'Public privacy regression tests failed.'}
& "$repo\tests\public_host_download_tests.ps1"
& "$PSScriptRoot\test-master-dsp-engine-headless.ps1" -Configuration $Configuration
& "$PSScriptRoot\test-analysis-revision-headless.ps1"
& "$PSScriptRoot\test-spectrum-resource-retry.ps1"
& python -B "$repo\tests\public_delivery_tests.py"
if ($LASTEXITCODE) {throw 'Public delivery finalization/privacy tests failed.'}
& python -B "$repo\tests\runtime_dependencies_tests.py"
if ($LASTEXITCODE) {throw 'Runtime dependency audit tests failed.'}
& "$repo\tests\toolchain_environment_tests.ps1"
& "$PSScriptRoot\test-constrained-shell.ps1"
& "$PSScriptRoot\test-system-ui-preferences.ps1"
& "$repo\tests\public_qa_tools_tests.ps1"
& "$PSScriptRoot\test-discord-relay-lifecycle.ps1"
& "$PSScriptRoot\test-opus-info-reader.ps1"
& "$PSScriptRoot\test-presence-cover-headless.ps1"
