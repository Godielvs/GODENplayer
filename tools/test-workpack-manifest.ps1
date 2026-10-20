param([ValidateSet('Release')][string]$Configuration = 'Release')

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$manifestPath = Join-Path $repo 'packaging\component-manifest.json'
$identityPath = Join-Path $repo 'src\component\build_identity.h'
$spectrumPath = Join-Path $repo "out\$Configuration\spectrum-analyzer-build.json"
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$identity = Get-Content -LiteralPath $identityPath -Raw
if ($identity -notmatch 'kVersion\[\]\s*=\s*"([^"]+)"' -or
        $matches[1] -ne $manifest.buildIdentity.version) {
    throw 'Canonical component version does not match the release manifest.'
}
if ($manifest.buildIdentity.releaseState -ne 'local-candidate-not-published') {
    throw 'Workpack output must remain an unpublished local candidate.'
}
if ($manifest.schema -ne 'goden.release-manifest.v2' -or
        $manifest.qaMutationHooks -ne $false -or $manifest.architecture -ne 'x86_64') {
    throw 'Expected the current public x64 manifest without QA mutation hooks.'
}
foreach ($entry in @(
        @{ item = $manifest.coreBinary; path = "out\$Configuration\foo_nekorox.dll" },
        @{ item = $manifest.analysisWorkerBinary; path = "out\$Configuration\nekorox-analysis.exe" },
        @{ item = $manifest.crtRendererBinary; path = "out\$Configuration\goden-crt-render.exe" },
        @{ item = $manifest.shaderRendererBinary; path = "out\$Configuration\goden-shader-render.exe" },
        @{ item = $manifest.columnsUiBinary; path = "out\$Configuration\foo_ui_columns.dll" },
        @{ item = $manifest.spectrumAnalyzerBinary; path = "out\$Configuration\foo_vis_spectrum_analyzer.dll" })) {
    $path = Join-Path $repo $entry.path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
            $entry.item.bytes -ne (Get-Item -LiteralPath $path).Length -or
            $entry.item.sha256 -ne (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash) {
        throw "Binary identity mismatch: $path"
    }
}
$spectrum = Get-Content -LiteralPath $spectrumPath -Raw | ConvertFrom-Json
$patchCount = @(Get-ChildItem -LiteralPath (
    Join-Path $repo 'third_party\spectrum-analyzer\patches') -Filter '*.patch').Count
if (@($spectrum.hashes.patches).Count -ne $patchCount -or
        @($spectrum.hashes.effectiveHeaders).Count -ne 6) {
    throw 'Spectrum provenance does not cover every effective patch/header.'
}
foreach ($inputRecord in @($spectrum.hashes.patches) + @($spectrum.hashes.effectiveHeaders)) {
    $inputPath = Join-Path $repo $inputRecord.path
    if ($inputRecord.sha256 -ne (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash) {
        throw "Spectrum input identity mismatch: $($inputRecord.path)"
    }
}
$renderHub = Get-Content -LiteralPath (
    Join-Path $repo 'src\component\render_device_hub.cpp') -Raw
if ($renderHub.Contains('recoveryChanged_.wait_for(lock, stopToken') -or
        -not $renderHub.Contains('recoveryChanged_.wait(lock, stopToken') -or
        -not $renderHub.Contains('entry.due < selectedDue')) {
    throw 'Render recovery returned to periodic idle polling or lost early-wake handling.'
}
Write-Host 'Workpack manifest and binary identity: PASS'
