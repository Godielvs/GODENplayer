param(
    [string]$LabRoot = '',
    [string]$BaseApp = '',
    [string]$FixtureRoot = '',
    [switch]$Force
)

# Build the C9 disposable laboratory: pristine portable base app + sanitized
# profile + the freshly built C9 binaries + synthetic bench media.
#
# Safety: this never reads the personal installed profile, never touches HKCU,
# and refuses any destination outside the repository's artifacts boundary. It
# does not install, promote, register or modify any user profile.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$artifacts = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
if (-not $LabRoot) { $LabRoot = Join-Path $repo 'artifacts\c9-lab' }
$lab = [IO.Path]::GetFullPath($LabRoot).TrimEnd('\')
if (-not ($lab + '\').StartsWith($artifacts, [StringComparison]::OrdinalIgnoreCase)) {
    throw "The C9 lab must live under artifacts: $lab"
}

# A complete portable foobar2000 host with an already-sanitized profile.
if (-not $BaseApp) { $BaseApp = Join-Path $repo 'artifacts\c3-qa\portable\app' }
$app = Join-Path $lab 'app'
$music = Join-Path $lab 'music'

if (-not (Test-Path -LiteralPath $BaseApp -PathType Container)) {
    throw "C9 lab base app is missing: $BaseApp"
}
if (-not (Test-Path -LiteralPath (Join-Path $BaseApp 'foobar2000.exe'))) {
    throw "C9 lab base app has no foobar2000.exe: $BaseApp"
}

if ($Force -and (Test-Path -LiteralPath $lab)) {
    Remove-Item -LiteralPath $lab -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $lab, $app, $music | Out-Null

if (-not (Test-Path -LiteralPath (Join-Path $app 'foobar2000.exe'))) {
    & robocopy.exe $BaseApp $app /E /COPY:DAT /DCOPY:T /R:2 /W:1 /NFL /NDL /NJH /NJS | Out-Null
    if ($LASTEXITCODE -gt 7) { throw "Base app copy failed: $LASTEXITCODE" }
}

$component = Join-Path $app 'profile\user-components-x64\foo_nekorox'
New-Item -ItemType Directory -Force -Path $component | Out-Null

# Replace only the C9 candidate binaries with the freshly built ones.
$built = [ordered]@{
    'foo_nekorox.dll'      = Join-Path $repo 'out\Release\foo_nekorox.dll'
    'nekorox-analysis.exe' = Join-Path $repo 'out\Release\nekorox-analysis.exe'
    'goden-shader-render.exe' = Join-Path $repo 'out\Release\goden-shader-render.exe'
}
$hashes = [ordered]@{}
foreach ($name in $built.Keys) {
    $source = $built[$name]
    if (-not (Test-Path -LiteralPath $source)) { throw "Built binary missing: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $component $name) -Force
    $hashes[$name] = (Get-FileHash -LiteralPath (Join-Path $component $name) -Algorithm SHA256).Hash
}
$spectrumSource = Join-Path $repo 'out\Release\foo_vis_spectrum_analyzer.dll'
if (Test-Path -LiteralPath $spectrumSource) {
    $spectrumTarget = Join-Path $app 'profile\user-components-x64\foo_vis_spectrum_analyzer'
    New-Item -ItemType Directory -Force -Path $spectrumTarget | Out-Null
    Copy-Item -LiteralPath $spectrumSource -Destination (Join-Path $spectrumTarget 'foo_vis_spectrum_analyzer.dll') -Force
    $hashes['foo_vis_spectrum_analyzer.dll'] = (Get-FileHash -LiteralPath (Join-Path $spectrumTarget 'foo_vis_spectrum_analyzer.dll') -Algorithm SHA256).Hash
}
$crt = Join-Path $repo 'out\Release\goden-crt-render.exe'
if (Test-Path -LiteralPath $crt) {
    Copy-Item -LiteralPath $crt -Destination (Join-Path $component 'goden-crt-render.exe') -Force
}

# Synthetic bench media: the five original C9 fixtures, never the user library.
# The packaged fixtures ship next to the work pack, which may sit outside the
# repository, so accept an explicit root and otherwise probe the known spots.
$fixtureCandidates = @()
if ($FixtureRoot) { $fixtureCandidates += $FixtureRoot }
$fixtureCandidates += @(
    (Join-Path $repo 'artifacts\c9-package\GODEN_C9_DELIVERY\fixtures\audio'),
    (Join-Path (Split-Path -Parent $repo) 'artifacts\c9-package\GODEN_C9_DELIVERY\fixtures\audio')
)
$fixtures = $fixtureCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Container } |
    Select-Object -First 1
$copied = @()
if ($fixtures) {
    Get-ChildItem -LiteralPath $fixtures -Filter *.wav | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $music $_.Name) -Force
        $copied += $_.Name
    }
}
if ($copied.Count -eq 0) {
    Write-Warning "C9 fixtures were not found; the lab has no bench media. Looked in: $($fixtureCandidates -join '; ')"
}

# The portable marker keeps every profile read/write inside the lab copy, so no
# user profile, %APPDATA% path or HKCU state is involved.
$portableMarker = Join-Path $app 'portable_mode_enabled'
Set-Content -LiteralPath $portableMarker -Value '1' -NoNewline -Encoding ascii

$result = [ordered]@{
    schema        = 'goden-c9-lab/1'
    labRoot       = $lab
    app           = $app
    music         = $music
    baseApp       = $BaseApp
    component     = $component
    portable      = (Get-Content -LiteralPath $portableMarker -Raw) -eq '1'
    binarySha256  = $hashes
    media         = $copied
    personalInstallUntouched = $true
    registryUntouched        = $true
}
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $lab 'lab.json') -Encoding utf8
$result | ConvertTo-Json -Depth 5
