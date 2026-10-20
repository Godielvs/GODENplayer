param(
    [ValidateSet('Release')][string]$Configuration = 'Release',
    [Parameter(Mandatory)][string]$CorrespondingSources,
    [string]$EvidenceRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if (-not $EvidenceRoot) {
    $EvidenceRoot = Join-Path $repo 'artifacts\workpack-v1\20260912T023210Z'
}
$evidence = [IO.Path]::GetFullPath($EvidenceRoot).TrimEnd('\')
$allowed = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
if (-not ($evidence + '\').StartsWith(
        $allowed, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Evidence output must remain below repo artifacts: $evidence"
}
$manifestPath = Join-Path $repo 'packaging\component-manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$version = [string]$manifest.buildIdentity.version
& (Join-Path $PSScriptRoot 'test-workpack-manifest.ps1') -Configuration $Configuration
& python -B (Join-Path $PSScriptRoot 'public_release.py') verify $CorrespondingSources --release-manifest $manifestPath
if ($LASTEXITCODE) { throw 'Matching corresponding sources are required for this package.' }
$stage = Join-Path $evidence 'candidate\foo_nekorox'
$package = Join-Path $evidence "candidate\foo_nekorox-$version.fb2k-component"
$files = [ordered]@{
    'foo_nekorox.dll' = Join-Path $repo "out\$Configuration\foo_nekorox.dll"
    'nekorox-analysis.exe' = Join-Path $repo "out\$Configuration\nekorox-analysis.exe"
    'goden-shader-render.exe' = Join-Path $repo "out\$Configuration\goden-shader-render.exe"
    'goden-crt-render.exe' = Join-Path $repo "out\$Configuration\goden-crt-render.exe"
    'LICENSE' = Join-Path $repo 'LICENSE'
    'LICENSE-SCOPE.md' = Join-Path $repo 'LICENSE-SCOPE.md'
    'licenses\KERR-RENDERER-COPYING.md' = Join-Path $repo 'src\shader-render\COPYING.md'
    'licenses\NPGS-GPL-3.0.txt' = Join-Path $repo 'LICENSES\NPGS-GPL-3.0.txt'
    'licenses\NPGS-NOTICE.txt' = Join-Path $repo 'LICENSES\NPGS-NOTICE.txt'
    'migration\install-native-shell-layout.js' = Join-Path $repo 'tools\install-native-shell-layout.js'
    'migration\fcl-codec.js' = Join-Path $repo 'tools\fcl-codec.js'
    'migration\nekorox-shell-v1.fcl' = Join-Path $repo 'config\layout\nekorox-v3-cohesive-bottom.fcl'
}
foreach ($path in $files.Values) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Package input is missing: $path"
    }
}
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null
foreach ($name in $files.Keys) {
    $destination = Join-Path $stage $name
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) |
        Out-Null
    Copy-Item -LiteralPath $files[$name] -Destination $destination -Force
}
Copy-Item -LiteralPath $manifestPath -Destination (
    Join-Path $stage 'component-manifest.json') -Force
@(
    "GODENplayer Core $version local workpack candidate",
    'Install only into a disposable foobar2000 x64 profile.',
    'The migration directory contains the idempotent old/V1/V2 to current PlayerShell configuration transform.',
    'Not published or deployed; qualification evidence is stored outside this component archive.',
    'Remove the component folder to roll back.'
    'Keep GODENplayer-corresponding-sources.zip beside this package; its hash is in PACKAGE_MANIFEST.json.'
) | Set-Content -LiteralPath (Join-Path $stage 'README.txt') -Encoding UTF8
if (Test-Path -LiteralPath $package) { Remove-Item -LiteralPath $package -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory(
    $stage, $package, [IO.Compression.CompressionLevel]::Optimal, $false)
$sourceSidecar = Join-Path $evidence 'candidate\GODENplayer-corresponding-sources.zip'
Copy-Item -LiteralPath $CorrespondingSources -Destination $sourceSidecar -Force
$proof = [ordered]@{
    schema = 'goden.workpack.component-package.v1'
    version = $version
    install_scope = 'disposable foobar2000 x64 profile only'
    path = $package
    bytes = (Get-Item -LiteralPath $package).Length
    sha256 = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash
    correspondingSourcesSha256 = (Get-FileHash -LiteralPath $sourceSidecar -Algorithm SHA256).Hash
    files = @($files.Keys | ForEach-Object {
        $path = Join-Path $stage $_
        [ordered]@{
            name = $_
            bytes = (Get-Item -LiteralPath $path).Length
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        }
    })
}
$proof | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (
    Join-Path $evidence 'candidate\PACKAGE_MANIFEST.json') -Encoding UTF8
Write-Host "Workpack component package: $package"
Write-Host "SHA256: $($proof.sha256)"
