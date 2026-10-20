param(
    [Parameter(Mandatory = $true)]
    [string]$PayloadStaging,
    [Parameter(Mandatory = $true)]
    [string]$FoobarInstaller,
    [string]$ReleaseManifest = '',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot "..\out\release"),
    [Parameter(Mandatory=$true)][string]$CorrespondingSources,
    [string]$InnoCompiler = (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
    [string]$SqliteExecutable = "sqlite3.exe",
    [string]$BuildDirectory = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$packaging = Join-Path $repo 'packaging\friend-installer'
if (-not $ReleaseManifest) {
    $ReleaseManifest = Join-Path $repo 'packaging\component-manifest.json'
}
$PayloadStaging = [IO.Path]::GetFullPath($PayloadStaging)
$FoobarInstaller = [IO.Path]::GetFullPath($FoobarInstaller)
$ReleaseManifest = [IO.Path]::GetFullPath($ReleaseManifest)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$artifactRoot = if ($BuildDirectory) {
    [IO.Path]::GetFullPath($BuildDirectory)
} else {
    Join-Path $repo 'artifacts\friend-installer'
}
$buildRoot = Join-Path $artifactRoot (
    'build-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
$payload = Join-Path $buildRoot 'payload'
$profile = Join-Path $payload 'profile'
$licenses = Join-Path $payload 'licenses'
$buildManifestPath = Join-Path $buildRoot 'friend-build-manifest.json'
$payloadManifestPath = Join-Path $buildRoot 'payload-manifest.json'
$stagingManifestPath = Join-Path $PayloadStaging 'staging-manifest.json'
$iscc = (Get-Command $InnoCompiler -ErrorAction Stop).Source
$sqlite = (Get-Command $SqliteExecutable -ErrorAction Stop).Source

$approvedColumnsSeedSha256 =
    'AF89E8C4C361DAB3717F49F9FCA6D0B319756100ED92CF1796563EA86AC059C8'
$approvedCleanConfigSha256 =
    'E5ED1163BC909B33BDC312280C1F5CC609BAD2527308619DE5C9A1E3AEA06967'
$approvedEmptyDspSha256 =
    'ACE810D7E2CBB4F8C40CE09DC8E191AE466ADB4E1A7D49C59F2215B411D38B05'

$profileAllowlist = @(
    'config.fb2k-dsp',
    'config.sqlite',
    'theme.fth',
    'configuration\foo_ui_columns.dll.cfg',
    'configuration\foo_vis_spectrum_analyzer.dll.cfg',
    'nekorox\presets\layout.fcl',
    'nekorox\presets\spectrum-golden.json',
    'user-components-x64\foo_nekorox\foo_nekorox.dll',
    'user-components-x64\foo_nekorox\nekorox-analysis.exe',
    'user-components-x64\foo_nekorox\goden-crt-render.exe',
    'user-components-x64\foo_nekorox\goden-shader-render.exe',
    'user-components-x64\foo_ui_columns\foo_ui_columns.dll',
    'user-components-x64\foo_vis_spectrum_analyzer\foo_vis_spectrum_analyzer.dll'
)

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Copy-Required([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Required installer input is missing: $Source"
    }
    New-Item -ItemType Directory -Force -Path (
        Split-Path -Parent $Destination) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Assert-File([string]$Path, [object]$Entry, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing ${Label}: $Path"
    }
    if ($null -eq $Entry -or -not $Entry.sha256 -or
            $null -eq $Entry.bytes) {
        throw "Manifest entry for ${Label} is incomplete."
    }
    $file = Get-Item -LiteralPath $Path
    $actualHash = Get-Sha256 $Path
    if ($file.Length -ne [int64]$Entry.bytes -or
            $actualHash -ne [string]$Entry.sha256) {
        throw "Frozen input mismatch for ${Label}: expected " +
            "$($Entry.bytes) / $($Entry.sha256), got " +
            "$($file.Length) / $actualHash."
    }
}

foreach ($required in @(
    $PayloadStaging, $FoobarInstaller, $ReleaseManifest,
    $stagingManifestPath, $iscc, $sqlite,
    (Join-Path $packaging 'NOTICE.txt'),
    (Join-Path $packaging 'third-party\FOOBAR2000-LICENSE.html'),
    (Join-Path $packaging 'third-party\COLUMNS-UI-GPL3.txt'),
    (Join-Path $packaging 'third-party\COLUMNS-UI-LGPL3.txt'),
    (Join-Path $packaging 'third-party\SOURCE-CORRESPONDENCE.txt')
)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing build dependency: $required"
    }
}

$release = Get-Content -LiteralPath $ReleaseManifest -Raw | ConvertFrom-Json
if ($release.schema -ne 'goden.release-manifest.v2' -or
        -not $release.buildIdentity.version) {
    throw 'Release manifest does not contain a canonical build version.'
}
$officialInstallerSha256='530E6C9DB829874BC250AF038C1D07F8B42FEF61C5DD99F7EDCDE6393A93EE23'
if ((Get-Sha256 $FoobarInstaller) -ne $officialInstallerSha256) { throw 'Official foobar2000 installer hash mismatch.' }
$signature=Get-AuthenticodeSignature $FoobarInstaller
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Illustrate Ltd') {
    throw 'Official installer signature is invalid or unexpected.'
}
if (!(Test-Path -LiteralPath $CorrespondingSources -PathType Leaf)) {throw 'Corresponding source archive is required.'}
& python (Join-Path $PSScriptRoot 'public_release.py') verify $CorrespondingSources --release-manifest $ReleaseManifest
if ($LASTEXITCODE -ne 0) {throw 'Corresponding source archive validation failed.'}
if ((Get-Item $FoobarInstaller).VersionInfo.ProductVersion -notmatch '^2\.25\.10(?:\D|$)') {throw 'Official host installer version mismatch.'}
$version = [string]$release.buildIdentity.version
if ($version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$') {
    throw "Unsafe canonical build version: $version"
}

$stagingManifest = Get-Content -LiteralPath $stagingManifestPath -Raw |
    ConvertFrom-Json
if ($stagingManifest.schema -ne 'goden.friend-staging.v1' -or
        -not $stagingManifest.cleanProfile -or
        -not $stagingManifest.nativeColumnsAccepted) {
    throw 'Payload staging is not a qualified pristine GODENplayer seed.'
}
$manifestPaths = @($stagingManifest.files | ForEach-Object {
    ([string]$_.path).Replace('/', '\')
})
$expectedPaths = @($profileAllowlist | ForEach-Object { "profile\$_" })
if (@(Compare-Object -ReferenceObject ($expectedPaths | Sort-Object) `
            -DifferenceObject ($manifestPaths | Sort-Object)).Count -ne 0) {
    throw 'Staging manifest does not contain exactly the profile allowlist.'
}
$actualPaths = @(Get-ChildItem -LiteralPath $PayloadStaging -Recurse -File |
    Where-Object { $_.FullName -ne $stagingManifestPath } |
    ForEach-Object {
        [IO.Path]::GetRelativePath($PayloadStaging, $_.FullName).Replace('/', '\')
    })
if (@(Compare-Object -ReferenceObject ($expectedPaths | Sort-Object) `
            -DifferenceObject ($actualPaths | Sort-Object)).Count -ne 0) {
    throw 'Payload staging contains missing or non-allowlisted files.'
}
foreach ($entry in $stagingManifest.files) {
    $relative = ([string]$entry.path).Replace('/', '\')
    Assert-File (Join-Path $PayloadStaging $relative) $entry $relative
}

$stagedProfile = Join-Path $PayloadStaging 'profile'
$stagedCore = Join-Path $stagedProfile `
    'user-components-x64\foo_nekorox\foo_nekorox.dll'
$stagedWorker = Join-Path $stagedProfile `
    'user-components-x64\foo_nekorox\nekorox-analysis.exe'
$stagedCrt = Join-Path $stagedProfile `
    'user-components-x64\foo_nekorox\goden-crt-render.exe'
$stagedShader = Join-Path $stagedProfile `
    'user-components-x64\foo_nekorox\goden-shader-render.exe'
$stagedSpectrum = Join-Path $stagedProfile `
    'user-components-x64\foo_vis_spectrum_analyzer\foo_vis_spectrum_analyzer.dll'
Assert-File $stagedCore $release.coreBinary 'GODENplayer core'
Assert-File $stagedWorker $release.analysisWorkerBinary 'analysis worker'
Assert-File $stagedCrt $release.crtRendererBinary 'CRT renderer'
Assert-File $stagedShader $release.shaderRendererBinary 'independent shader renderer'
Assert-File $stagedSpectrum $release.spectrumAnalyzerBinary 'spectrum analyzer'

$columnsDll = Join-Path $stagedProfile `
    'user-components-x64\foo_ui_columns\foo_ui_columns.dll'
$columnsSeed = Join-Path $stagedProfile `
    'configuration\foo_ui_columns.dll.cfg'
$cleanConfig = Join-Path $stagedProfile 'config.sqlite'
$emptyDsp = Join-Path $stagedProfile 'config.fb2k-dsp'
Assert-File $columnsDll $release.columnsUiBinary 'modified Columns UI'
if ((Get-Sha256 $columnsSeed) -ne $approvedColumnsSeedSha256 -or
        (Get-Sha256 $cleanConfig) -ne $approvedCleanConfigSha256 -or
        (Get-Sha256 $emptyDsp) -ne $approvedEmptyDspSha256) {
    throw 'Pristine seed or approved third-party component drifted.'
}
$quickCheck = (& $sqlite $cleanConfig 'PRAGMA quick_check;') -join "`n"
if ($LASTEXITCODE -ne 0 -or $quickCheck.Trim() -ne 'ok') {
    throw "Pristine config.sqlite failed integrity validation: $quickCheck"
}
$activeUi = (& $sqlite $cleanConfig @"
SELECT upper(hex(value))
FROM configBlobs
WHERE name = 'cfg_var.094A28DA-C6E8-4392-BAF1-AD0F29E5B303';
"@) -join ''
$nullValues = (& $sqlite $cleanConfig @"
SELECT
  (SELECT count(*) FROM config WHERE value IS NULL) +
  (SELECT count(*) FROM configInts WHERE value IS NULL) +
  (SELECT count(*) FROM configReals WHERE value IS NULL) +
  (SELECT count(*) FROM configStrings WHERE value IS NULL) +
  (SELECT count(*) FROM configBlobs WHERE value IS NULL);
"@) -join ''
if ($LASTEXITCODE -ne 0 -or
        $activeUi.Trim() -ne '240A2DF1A4A8184696596F66DE067524' -or
        $nullValues.Trim() -ne '0') {
    throw 'Pristine config.sqlite is structurally valid but not a clean ' +
        'Columns UI seed.'
}

New-Item -ItemType Directory -Force -Path `
    $profile, $licenses, $OutputDirectory | Out-Null
foreach ($relative in $profileAllowlist) {
    Copy-Required (Join-Path $stagedProfile $relative) `
        (Join-Path $profile $relative)
}

$privacyManifest = Join-Path $buildRoot 'friend-profile-privacy.json'
& node (Join-Path $PSScriptRoot 'sanitize-friend-profile.js') `
    $profile $privacyManifest
if ($LASTEXITCODE -ne 0) {
    throw 'Pristine friend profile privacy verification failed.'
}
$columnsSeedManifest = Join-Path $buildRoot 'columns-config-seed.json'
& node (Join-Path $PSScriptRoot 'verify-columns-config-seed.js') `
    (Join-Path $profile 'configuration\foo_ui_columns.dll.cfg') `
    (Join-Path $profile 'nekorox\presets\layout.fcl') `
    $columnsSeedManifest $approvedColumnsSeedSha256
if ($LASTEXITCODE -ne 0) {
    throw 'Columns UI native seed verification failed.'
}

Copy-Required (Join-Path $packaging 'NOTICE.txt') `
    (Join-Path $licenses 'GODEN-NOTICE.txt')
Copy-Required $ReleaseManifest `
    (Join-Path $licenses 'GODEN-RELEASE-MANIFEST.json')
Copy-Required (Join-Path $packaging 'third-party\FOOBAR2000-LICENSE.html') `
    (Join-Path $licenses 'FOOBAR2000-LICENSE.html')
Copy-Required (Join-Path $packaging 'third-party\COLUMNS-UI-GPL3.txt') `
    (Join-Path $licenses 'COLUMNS-UI-GPL3.txt')
Copy-Required (Join-Path $packaging 'third-party\COLUMNS-UI-LGPL3.txt') `
    (Join-Path $licenses 'COLUMNS-UI-LGPL3.txt')
Copy-Required (Join-Path $packaging 'third-party\SOURCE-CORRESPONDENCE.txt') `
    (Join-Path $licenses 'SOURCE-CORRESPONDENCE.txt')
Copy-Required (Join-Path $repo `
    'out\third_party\spectrum-analyzer\foo_vis_spectrum_analyzer\LICENSE') `
    (Join-Path $licenses 'SPECTRUM-ANALYZER-LICENSE.txt')
Copy-Required (Join-Path $repo 'vendor\libsonare\LICENSE') `
    (Join-Path $licenses 'LIBSONARE-LICENSE.txt')
Copy-Required (Join-Path $repo 'vendor\libsonare\NOTICE') `
    (Join-Path $licenses 'LIBSONARE-NOTICE.txt')
Copy-Required (Join-Path $repo 'vendor\zstd\LICENSE') `
    (Join-Path $licenses 'ZSTD-LICENSE.txt')
Copy-Required (Join-Path $repo 'vendor\eigen\COPYING.MPL2') `
    (Join-Path $licenses 'EIGEN-MPL2.txt')
Copy-Required (Join-Path $repo 'vendor\sqlite\README.md') `
    (Join-Path $licenses 'SQLITE-NOTICE.txt')

if (Test-Path (Join-Path $repo 'LICENSES')) {
    Copy-Item (Join-Path $repo 'LICENSES') $licenses -Recurse
}
Copy-Required (Join-Path $repo 'src\shader-render\COPYING.md') `
    (Join-Path $licenses 'KERR-RENDERER-COPYING.md')
$forbiddenNames = @(
    'metadb.sqlite', 'library-v2.0', 'playlists-v2.0', 'yttm',
    'nekorox.db', 'minibar.db', 'lyrics', 'history', 'notes', 'running',
    'nobadshutdown', 'startup.incomplete', 'foo_openlyrics.dll'
)
$forbidden = @(Get-ChildItem -LiteralPath $payload -Recurse -Force |
    Where-Object { $forbiddenNames -contains $_.Name })
if ($forbidden.Count -gt 0) {
    throw "Forbidden personal/runtime artifacts entered payload: " +
        (($forbidden | ForEach-Object FullName) -join ', ')
}

$sourcesDestination = Join-Path $OutputDirectory 'GODENplayer-corresponding-sources.zip'
if (![IO.Path]::GetFullPath($CorrespondingSources).Equals(
        [IO.Path]::GetFullPath($sourcesDestination), [StringComparison]::OrdinalIgnoreCase)) {
    Copy-Item -LiteralPath $CorrespondingSources -Destination $sourcesDestination -Force
}
$payloadFiles = @(Get-ChildItem -LiteralPath $payload -Recurse -File |
    Sort-Object FullName)
$payloadRecords = @($payloadFiles | ForEach-Object {
    [ordered]@{
        path = [IO.Path]::GetRelativePath($payload, $_.FullName).Replace('\', '/')
        bytes = $_.Length
        sha256 = Get-Sha256 $_.FullName
    }
})
$payloadManifest = [ordered]@{
    schema = 'goden.friend-payload.v1'
    version = $version
    correspondingSourcesSha256 = Get-Sha256 $CorrespondingSources
    releaseManifestSha256 = Get-Sha256 $ReleaseManifest
    stagingManifestSha256 = Get-Sha256 $stagingManifestPath
    foobarInstallerSha256 = Get-Sha256 $FoobarInstaller
    files = $payloadRecords
}
$payloadManifest | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath $payloadManifestPath -Encoding UTF8
Copy-Required $payloadManifestPath `
    (Join-Path $licenses 'GODEN-PAYLOAD-MANIFEST.json')

# Recompute after the payload manifest itself has become part of the archive.
$payloadFiles = @(Get-ChildItem -LiteralPath $payload -Recurse -File |
    Sort-Object FullName)
$coreHash = Get-Sha256 $stagedCore
$workerHash = Get-Sha256 $stagedWorker
$crtHash = Get-Sha256 $stagedCrt
$shaderHash = Get-Sha256 $stagedShader
$spectrumHash = Get-Sha256 $stagedSpectrum
$iss = Join-Path $packaging 'NekoRoX-Friend.iss'
& $iscc "/DPayloadRoot=$payload" "/DFoobarInstaller=$FoobarInstaller" `
    "/DCoreSha256=$coreHash" "/DWorkerSha256=$workerHash" `
    "/DCrtSha256=$crtHash" `
    "/DShaderSha256=$shaderHash" `
    "/DSpectrumSha256=$spectrumHash" "/DColumnsSha256=$($release.columnsUiBinary.sha256)" "/DOutputDir=$OutputDirectory" `
    "/DBuildVersion=$version" $iss
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compilation failed with exit code $LASTEXITCODE."
}

$setup = Join-Path $OutputDirectory 'GODENplayer-Beta-Setup-x64.exe'
if (-not (Test-Path -LiteralPath $setup -PathType Leaf)) {
    throw "Setup output was not created: $setup"
}
$setupHash = Get-Sha256 $setup
$hashFile = Join-Path $OutputDirectory `
    'GODENplayer-Beta-Setup-x64.sha256.txt'
Set-Content -LiteralPath $hashFile -Encoding ASCII `
    -Value "$setupHash  GODENplayer-Beta-Setup-x64.exe"

[int64]$payloadBytes = ($payloadFiles | Measure-Object Length -Sum).Sum
$buildManifest = [ordered]@{
    schema = 'goden.friend-installer-build.v3'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    version = $version
    installerSeedRevision = 2
    beta = $true
    sourceApp = $null
    payloadStaging = "clean-allowlisted-seed"
    payloadStagingWasModified = $false
    payloadFiles = $payloadFiles.Count
    payloadBytes = $payloadBytes
    payloadManifestSha256 = Get-Sha256 $payloadManifestPath
    correspondingSourcesSha256 = Get-Sha256 $CorrespondingSources
    releaseManifestSha256 = Get-Sha256 $ReleaseManifest
    stagingManifestSha256 = Get-Sha256 $stagingManifestPath
    privacyManifest = "friend-profile-privacy.json"
    columnsSeedManifest = "columns-config-seed.json"
    configSeedSha256 = Get-Sha256 $cleanConfig
    columnsConfigSeedSha256 = Get-Sha256 $columnsSeed
    setup = [IO.Path]::GetFileName($setup)
    setupBytes = (Get-Item -LiteralPath $setup).Length
    setupSha256 = $setupHash
    foobarInstaller = [IO.Path]::GetFileName($FoobarInstaller)
    foobarInstallerSha256 = Get-Sha256 $FoobarInstaller
    officialInstallerEmbeddedIntact = $true
    officialHostNotEmbedded = $true
    installedHostExpectedSha256 = '653CC120C146AAAE9E6DB9B6F19E5A1588407B8940BC1521F0CED739FF8924B0'
    hostExecutablePatched = $false
    hostDpiProvidedBy = "Columns UI bounded PMv2 patch"
    openLyricsShipped = $false
    embeddedComponents = [ordered]@{
        fooNekoroxSha256 = $coreHash
        analysisWorkerSha256 = $workerHash
        crtRendererSha256 = $crtHash
        shaderRendererSha256 = $shaderHash
        spectrumAnalyzerSha256 = $spectrumHash
        fooUiColumnsSha256 = Get-Sha256 $columnsDll
    }
}
$buildManifest | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath $buildManifestPath -Encoding UTF8
Copy-Item -LiteralPath $buildManifestPath -Destination (
    Join-Path $OutputDirectory 'GODENplayer-Beta-Setup-x64.manifest.json') -Force

Write-Host "GODENplayer Beta installer: $setup"
Write-Host "SHA-256: $setupHash"
Write-Host "Build evidence: $buildManifestPath"
