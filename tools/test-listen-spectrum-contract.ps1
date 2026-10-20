param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))

function Require-Text(
    [string]$Path,
    [string[]]$Needles,
    [string]$Contract) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Contract input is missing: $Path"
    }
    $text = Get-Content -LiteralPath $Path -Raw
    foreach ($needle in $Needles) {
        if (-not $text.Contains($needle)) {
            throw "$Contract is incomplete: $needle"
        }
    }
}

Require-Text `
    (Join-Path $repo 'src\core\include\nekorox\realtime_visual_model.h') `
    @('kSpectrumFftBins = kAnalysisFftSize / 2U + 1U',
      'spectrumPowerLeft{}', 'spectrumPowerRight{}',
      'spectrumPowerStereo{}', 'spectrumPowerMid{}',
      'spectrumZeroPadded', 'spectralCentroidHz',
      'spectralRolloff85Hz', 'spectralConfidence') `
    'Calibrated stereo spectrum snapshot'

Require-Text `
    (Join-Path $repo 'src\core\src\realtime_visual_model.cpp') `
    @('updateCalibratedSpectrum()', 'amplitude * amplitude',
      'transform(spectrumHistoryLeft_', 'snapshot_.spectrumPowerStereo[bin] = 0.5f',
      'totalPower * 0.85', 'previousSpectralConfidence_') `
    'Time-based spectral analysis and markers'

Require-Text `
    (Join-Path $repo 'src\component\listen_visual_renderer.cpp') `
    @('DYNAMICS \u00B7 8 s', 'dynamics->count', 'dynamics->sourceQpc - bin.endQpc',
      'PEAK %.1f', 'RMS %.1f', 'CREST %.1f dB',
      'SPECTROGRAM \u00B7 4 s',
      'scopeLightPass_.draw(', 'softwareRasterizer_') `
    'Listen Grid R7 Dynamics and scope presentation'

Require-Text `
    (Join-Path $repo 'src\core\include\nekorox\realtime_visual_model.h') `
    @('kSpectrogramBins = 256') `
    'R7 256-band spectrogram'

Require-Text `
    (Join-Path $repo 'src\component\shaders\listen_scope_light.hlsl') `
    @('float2 MaskPS', 'float2 BlurPS', 'float4 CompositePS',
      '1.7 * nearCoverage + 0.6 * wideCoverage',
      'core * (1.05 + 0.35 * density)',
      '0.42 * nearCoverage + 0.14 * wideCoverage',
      '1.0 - exp(', 'opticalStrength < 0.0') `
    'Listen Grid L1 spatial scope shader'

Require-Text `
    (Join-Path $repo 'src\component\scope_light_pass.cpp') `
    @('D3D11_BLEND_OP_MAX', 'DXGI_FORMAT_R16G16_FLOAT',
      'ScopeLightPass::draw', 'maskRevision_ != visualRevision',
      '2.0f * dpi / 96.0f * scale',
      '8.0f * dpi / 96.0f * scale',
      'swapChain->GetBuffer(', 'surfaceSrgb_') `
    'Listen Grid L1 scope GPU pass'

Require-Text `
    (Join-Path $repo 'src\component\foo_nekorox.vcxproj') `
    @('listen_scope_light.hlsl', 'scope_mask_vs.h',
      'scope_blur_ps.h', 'scope_composite_ps.h') `
    'Listen Grid L1 build-time shader compilation'

$renderer = Get-Content -LiteralPath (
    Join-Path $repo 'src\component\listen_visual_renderer.cpp') -Raw
if ($renderer.Contains('ROLLOFF 85%') -and
    -not $renderer.Contains('if (false && markerConfidence')) {
    throw 'Listen Grid L1 unexpectedly renders analysis markers.'
}

$listenWindow = Get-Content -LiteralPath (
    Join-Path $repo 'src\component\listen_window.cpp') -Raw
$listenHeader = Get-Content -LiteralPath (
    Join-Path $repo 'src\component\listen_window.h') -Raw
if ($renderer.Contains('LargeScope') -or
    $listenWindow.Contains('LargeScope') -or
    $listenHeader.Contains('LargeScope')) {
    throw 'Listen Grid L1 reintroduced the forbidden LargeScope path.'
}

Write-Host 'GODENplayer Listen Grid R7 dynamics/GPU contract: PASS'
