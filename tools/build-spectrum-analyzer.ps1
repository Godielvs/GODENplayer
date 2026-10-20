param(
    [ValidateSet('Release')]
    [string]$Configuration = 'Release',
    [string]$PythonExecutable = 'python.exe',
    [string]$SdkRoot = (Join-Path $PSScriptRoot '..\out\dependencies\foobar2000-sdk\SDK-2025-03-07'),
    [string]$ToolchainRoot = (Join-Path $PSScriptRoot '..\out\toolchains'),
    [switch]$VerifyDeterminism,
    [ValidateRange(1, 1)]
    [int]$Workers = 1
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
& $PythonExecutable -B (Join-Path $PSScriptRoot 'verify-build-inputs.py')
if ($LASTEXITCODE) {throw 'Pinned build input verification failed.'}
$definitionRoot = Join-Path $repo 'third_party\spectrum-analyzer'
$lockPath = Join-Path $definitionRoot 'upstream.lock.json'
$patchPaths = @(
    (Join-Path $definitionRoot 'patches\0001-nekorox-adaptive-scheduler.patch'),
    (Join-Path $definitionRoot 'patches\0002-goden-root-visibility-wake.patch'),
    (Join-Path $definitionRoot 'patches\0003-goden-spectrum-color-curve-v2.patch'),
    (Join-Path $definitionRoot 'patches\0004-goden-daw-visual-source.patch'),
    (Join-Path $definitionRoot 'patches\0005-goden-display-refresh.patch'),
    (Join-Path $definitionRoot 'patches\0006-goden-optical-spectrum-bars.patch'),
    (Join-Path $definitionRoot 'patches\0007-goden-adaptive-spectrum-sampling.patch'),
    (Join-Path $definitionRoot 'patches\0008-goden-independent-note-background.patch'),
    (Join-Path $definitionRoot 'patches\0009-goden-constant-physical-bar-gap.patch'),
    (Join-Path $definitionRoot 'patches\0010-goden-pause-release.patch'),
    (Join-Path $definitionRoot 'patches\0011-goden-stop-release.patch'),
    (Join-Path $definitionRoot 'patches\0012-goden-coalesced-stop-release.patch'),
    (Join-Path $definitionRoot 'patches\0013-goden-release-deadline.patch')
    (Join-Path $definitionRoot 'patches\0014-goden-portable-preset-path.patch')
    (Join-Path $definitionRoot 'patches\0015-goden-artwork-reuse.patch')
    (Join-Path $definitionRoot 'patches\0016-goden-render-caches.patch')
    (Join-Path $definitionRoot 'patches\0017-goden-mono-spectrum.patch')
    (Join-Path $definitionRoot 'patches\0018-goden-root-gate-resize-recovery.patch')
)
$themeProviderHeader = Join-Path $repo 'src\component\nekorox_theme_provider.h'
$dawVisualHeader = Join-Path $definitionRoot 'GodenDawVisualSource.h'
$dawVisualProducerHeader = Join-Path $repo 'src\component\daw_visual_bridge.h'
$renderCacheHeaders = @(
    (Join-Path $definitionRoot 'NekoRenderSampling.h'),
    (Join-Path $definitionRoot 'NekoAxisText.h'),
    (Join-Path $definitionRoot 'NekoBarBatch.h')
)
$workRoot = Join-Path $repo 'out\third_party\spectrum-analyzer'
$sourceRoot = Join-Path $workRoot 'foo_vis_spectrum_analyzer'
$columnsRoot = Join-Path $workRoot 'columns_ui_sdk-source'
$thirdPartyRoot = Join-Path $workRoot '3rdParty'
$downloadsRoot = Join-Path $workRoot 'downloads'
$wtlZip = Join-Path $downloadsRoot 'WTL10_10320_Release.zip'
$wtlRoot = Join-Path $workRoot 'WTL10_10320'
$buildOut = Join-Path $workRoot "out\x64\$Configuration"
$finalDll = Join-Path $repo "out\$Configuration\foo_vis_spectrum_analyzer.dll"
$manifestPath = Join-Path $repo "out\$Configuration\spectrum-analyzer-build.json"
function Get-Sha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Invoke-Git([string[]]$Arguments) {
    & git @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Git failed with exit code ${LASTEXITCODE}: $($Arguments -join ' ')"
    }
}

function Get-GitText([string[]]$Arguments) {
    $value = & git @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Git query failed with exit code ${LASTEXITCODE}: $($Arguments -join ' ')"
    }
    return ($value | Out-String).Trim()
}

function Ensure-GitCheckout(
    [string]$Path,
    [string]$Url,
    [string]$Tag,
    [string]$Commit
) {
    if (-not (Test-Path -LiteralPath (Join-Path $Path '.git'))) {
        if (Test-Path -LiteralPath $Path) {
            throw "Refusing to replace non-Git dependency directory: $Path"
        }
        Invoke-Git @('clone', '--depth', '1', '--branch', $Tag, $Url, $Path)
    }

    $actual = Get-GitText @('-C', $Path, 'rev-parse', 'HEAD')
    if ($actual -ne $Commit) {
        throw "Pinned dependency mismatch at ${Path}: expected $Commit, got $actual"
    }
}

function Ensure-Junction([string]$Path, [string]$Target) {
    $targetFull = [IO.Path]::GetFullPath($Target).TrimEnd('\')
    if (Test-Path -LiteralPath $Path) {
        $item = Get-Item -LiteralPath $Path -Force
        $actualTargets = @($item.Target) | ForEach-Object {
            [IO.Path]::GetFullPath([string]$_).TrimEnd('\')
        }
        if ($item.LinkType -ne 'Junction' -or
            -not ($actualTargets -contains $targetFull)) {
            throw "Dependency path exists but is not the expected junction: $Path"
        }
        return
    }
    New-Item -ItemType Junction -Path $Path -Target $targetFull | Out-Null
}

function Import-VsDevEnvironment {
    & "$PSScriptRoot/import-v142-environment.ps1" | Out-Null
}

foreach ($required in @($lockPath, $themeProviderHeader, $dawVisualHeader,
        $dawVisualProducerHeader, $SdkRoot) + $patchPaths + $renderCacheHeaders) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Pinned spectrum build input is missing: $required"
    }
}

# The Spectrum fork is a separate DLL and therefore carries a small reader for
# the DAW shared-memory protocol. Fail the build if its public identity drifts
# from the producer again; a V1 reader with a V2 producer otherwise fails as a
# perfectly healthy but permanently blank visualizer.
$dawProducerContract = Get-Content -LiteralPath $dawVisualProducerHeader -Raw
$dawConsumerContract = Get-Content -LiteralPath $dawVisualHeader -Raw
foreach ($contractToken in @(
        'Local\\GodenPlayer.DawPreview.Visual.v2',
        'Local\\GodenPlayer.DawPreview.Wake.v2',
        '0x32564447U')) {
    if (-not $dawProducerContract.Contains($contractToken) -or
            -not $dawConsumerContract.Contains($contractToken)) {
        throw "DAW visual protocol mismatch between producer and Spectrum reader: $contractToken"
    }
}

$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $workRoot, $thirdPartyRoot, $downloadsRoot, $ToolchainRoot | Out-Null

Ensure-GitCheckout $sourceRoot $lock.spectrumAnalyzer.url `
    $lock.spectrumAnalyzer.tag $lock.spectrumAnalyzer.commit
Ensure-GitCheckout $columnsRoot $lock.columnsUiSdk.url `
    $lock.columnsUiSdk.tag $lock.columnsUiSdk.commit

# Patches may touch the same file. Testing an early patch in reverse while a
# later patch is still present can therefore produce a false "not applied".
# Normalize the pinned checkout by removing known patches in reverse order,
# then apply the complete series in its canonical forward order.
for ($index = $patchPaths.Count - 1; $index -ge 0; --$index) {
    $patchPath = $patchPaths[$index]
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'SilentlyContinue'
    & git -C $sourceRoot apply --reverse --check $patchPath 2>$null
    $patchAlreadyApplied = $LASTEXITCODE -eq 0
    $ErrorActionPreference = $savedErrorActionPreference
    if ($patchAlreadyApplied) {
        Invoke-Git @('-C', $sourceRoot, 'apply', '--reverse', $patchPath)
    }
}
foreach ($patchPath in $patchPaths) {
    Invoke-Git @('-C', $sourceRoot, 'apply', '--check', $patchPath)
    Invoke-Git @('-C', $sourceRoot, 'apply', $patchPath)
}
Copy-Item -LiteralPath $themeProviderHeader `
    -Destination (Join-Path $sourceRoot 'NekoThemeProvider.h') -Force
Copy-Item -LiteralPath $dawVisualHeader `
    -Destination (Join-Path $sourceRoot 'GodenDawVisualSource.h') -Force
foreach ($header in $renderCacheHeaders) {
    Copy-Item -LiteralPath $header `
        -Destination (Join-Path $sourceRoot (Split-Path -Leaf $header)) -Force
}

if (-not (Test-Path -LiteralPath $wtlZip)) {
    & curl.exe -L --fail --silent --show-error $lock.wtl.url -o $wtlZip
    if ($LASTEXITCODE -ne 0) { throw 'Failed to download pinned WTL release.' }
}
$wtlHash = Get-Sha256 $wtlZip
if ($wtlHash -ne $lock.wtl.sha256) {
    throw "WTL archive hash mismatch: expected $($lock.wtl.sha256), got $wtlHash"
}
if (-not (Test-Path -LiteralPath (Join-Path $wtlRoot 'Include\atlapp.h'))) {
    if (Test-Path -LiteralPath $wtlRoot) {
        throw "Incomplete WTL extraction exists and will not be replaced automatically: $wtlRoot"
    }
    Expand-Archive -LiteralPath $wtlZip -DestinationPath $wtlRoot
}

$atlPortable = Join-Path $ToolchainRoot 'v142-atl-portable'
$atlRoot = Join-Path $atlPortable "Contents\VC\Tools\MSVC\$($lock.atl.toolsetDirectory)\atlmfc"
if ($env:GODEN_ATL_ROOT) {$atlRoot=[IO.Path]::GetFullPath($env:GODEN_ATL_ROOT)}
elseif ($env:VCToolsInstallDir -and (Test-Path (Join-Path $env:VCToolsInstallDir 'atlmfc/include/atlbase.h'))) {$atlRoot=Join-Path $env:VCToolsInstallDir 'atlmfc'}
if (-not (Test-Path -LiteralPath (Join-Path $atlRoot 'include\atlbase.h')) -or
    -not (Test-Path -LiteralPath (Join-Path $atlRoot 'lib\x64\atls.lib'))) {
    $bootstrapper = Join-Path $ToolchainRoot 'vs_BuildTools_17.exe'
    $layout = Join-Path $ToolchainRoot 'vs2022-v142-atl-layout'
    if (-not (Test-Path -LiteralPath $bootstrapper)) {
        throw "Visual Studio ATL setup is required. Download and verify the signed Build Tools bootstrapper separately, then place it at: $bootstrapper"
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $bootstrapper
    if ($signature.Status -ne 'Valid' -or
            $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') {
        throw "Refusing untrusted Visual Studio bootstrapper: signature status=$($signature.Status) signer=$($signature.SignerCertificate.Subject)"
    }
    if (-not $lock.atl.bootstrapperSha256) {
        throw 'ATL lock is missing bootstrapperSha256; refusing to execute a movable download.'
    }
    $bootstrapperHash = Get-Sha256 $bootstrapper
    if ($bootstrapperHash -ne $lock.atl.bootstrapperSha256) {
        throw "Visual Studio bootstrapper hash mismatch: expected $($lock.atl.bootstrapperSha256), got $bootstrapperHash"
    }

    $headerPayload = Join-Path $layout "Microsoft.VC.14.29.16.11.ATL.Headers.base,version=$($lock.atl.payloadVersion)\payload.vsix"
    $sourcePayload = Join-Path $layout "Microsoft.VC.14.29.16.11.ATL.Source.base,version=$($lock.atl.payloadVersion)\payload.vsix"
    $x64Payload = Join-Path $layout "Microsoft.VC.14.29.16.11.ATL.X64.base,version=$($lock.atl.payloadVersion)\payload.vsix"
    if (-not (Test-Path -LiteralPath $headerPayload) -or
        -not (Test-Path -LiteralPath $sourcePayload) -or
        -not (Test-Path -LiteralPath $x64Payload)) {
        & $bootstrapper --layout $layout --add $lock.atl.component `
            --lang en-US --quiet --wait --norestart
    }
    foreach ($payload in @($headerPayload, $sourcePayload, $x64Payload)) {
        if (-not (Test-Path -LiteralPath $payload)) {
            throw "ATL layout did not contain the required payload: $payload"
        }
    }

    New-Item -ItemType Directory -Force -Path $atlPortable | Out-Null
    foreach ($payload in @($headerPayload, $sourcePayload, $x64Payload)) {
        & tar.exe -xf $payload -C $atlPortable
        if ($LASTEXITCODE -ne 0) { throw "Failed to extract ATL payload: $payload" }
    }
}

Ensure-Junction (Join-Path $workRoot 'sdk') $SdkRoot
Ensure-Junction (Join-Path $thirdPartyRoot 'columns_ui_sdk') $columnsRoot
Ensure-Junction (Join-Path $thirdPartyRoot 'columns_ui-sdk') $columnsRoot
Ensure-Junction (Join-Path $thirdPartyRoot 'WTL10_10320') $wtlRoot
Ensure-Junction (Join-Path $thirdPartyRoot 'pfc') (Join-Path $SdkRoot 'pfc')
Ensure-Junction (Join-Path $thirdPartyRoot 'foobar2000') (Join-Path $SdkRoot 'foobar2000')
Copy-Item -LiteralPath (Join-Path $definitionRoot 'Directory.Build.targets') `
    -Destination (Join-Path $workRoot 'Directory.Build.targets') -Force

Import-VsDevEnvironment
$msbuild = & "$PSScriptRoot\resolve-msbuild.ps1"
$project = Join-Path $sourceRoot 'foo_vis_spectrum_analyzer.vcxproj'
$builtDll = Join-Path $buildOut 'foo_vis_spectrum_analyzer.dll'

function Invoke-SpectrumBuild([string]$BuildTarget) {
    & $msbuild $project /m:1 /nodeReuse:false "/t:$BuildTarget" /p:Configuration=$Configuration `
        /p:CL_MPCount=$Workers /p:MultiProcMaxCount=$Workers `
        /p:Platform=x64 "/p:PlatformToolset=$($lock.platformToolset)" `
        "/p:WindowsTargetPlatformVersion=$($lock.windowsSdk)" `
        "/p:NekoATLRoot=$atlRoot" `
        "/p:NekoWTLRoot=$(Join-Path $wtlRoot 'Include')" `
        "/p:SolutionDir=$workRoot\" "/p:OutDir=$buildOut\" `
        /p:PostBuildEventUseInBuild=false /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) {
        throw "Spectrum Analyzer fork build failed with exit code $LASTEXITCODE."
    }
}

$determinismVerified = $false
$sourceFingerprint = & $PythonExecutable -B (Join-Path $PSScriptRoot 'public_release.py') dependency-fingerprint spectrum
if ($LASTEXITCODE) {throw 'Spectrum pre-build source snapshot failed.'}
if ($VerifyDeterminism) {
    Invoke-SpectrumBuild 'Rebuild'
    $firstHash = Get-Sha256 $builtDll
    Invoke-SpectrumBuild 'Rebuild'
    $secondHash = Get-Sha256 $builtDll
    if ($firstHash -ne $secondHash) {
        throw "Spectrum Analyzer build is not deterministic: $firstHash != $secondHash"
    }
    $determinismVerified = $true
}
else {
    Invoke-SpectrumBuild 'Build'
}

if (-not (Test-Path -LiteralPath $builtDll)) {
    throw "Spectrum Analyzer build did not produce the expected DLL: $builtDll"
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $finalDll) | Out-Null
Copy-Item -LiteralPath $builtDll -Destination $finalDll -Force

$fileVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($finalDll)
$manifest = [ordered]@{
    sourceTreeSha256 = $sourceFingerprint
    schema = 'nekorox.spectrum-build.v1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    forkVersion = $lock.forkVersion
    fileVersion = $fileVersion.FileVersion
    productVersion = $fileVersion.ProductVersion
    architecture = 'x86_64'
    upstreamCommit = $lock.spectrumAnalyzer.commit
    columnsUiSdkCommit = $lock.columnsUiSdk.commit
    foobarSdk = $lock.foobarSdk
    platformToolset = $lock.platformToolset
    windowsSdk = $lock.windowsSdk
    determinismVerified = $determinismVerified
    themeProviderHeaderSha256 = Get-Sha256 $themeProviderHeader
    hashes = [ordered]@{
        patches = @($patchPaths | ForEach-Object {
            [ordered]@{ path = [IO.Path]::GetRelativePath($repo, $_); sha256 = Get-Sha256 $_ }
        })
        effectiveHeaders = @(
            [ordered]@{ path = [IO.Path]::GetRelativePath($repo, $themeProviderHeader); sha256 = Get-Sha256 $themeProviderHeader },
            [ordered]@{ path = [IO.Path]::GetRelativePath($repo, $dawVisualHeader); sha256 = Get-Sha256 $dawVisualHeader },
            [ordered]@{ path = [IO.Path]::GetRelativePath($repo, $dawVisualProducerHeader); sha256 = Get-Sha256 $dawVisualProducerHeader }
        ) + @($renderCacheHeaders | ForEach-Object {
            [ordered]@{ path = [IO.Path]::GetRelativePath($repo, $_); sha256 = Get-Sha256 $_ }
        })
        wtlArchive = $wtlHash
        dll = Get-Sha256 $finalDll
    }
    output = [IO.Path]::GetRelativePath($repo,$finalDll)
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
& $PythonExecutable -B (Join-Path $PSScriptRoot 'public_release.py') seal-dependency spectrum
if ($LASTEXITCODE) {throw 'Spectrum source receipt failed.'}

Write-Host "Built Spectrum Analyzer $($lock.forkVersion): $finalDll"
Write-Host "Build manifest: $manifestPath"
