"use strict";

const fs = require("fs");
const path = require("path");
const { parseFcl, parseLayout } = require("./fcl-codec");

const repo = path.resolve(__dirname, "..");
if (!fs.readFileSync(path.join(repo, "config/spectrum/nekorox-golden.json")).equals(
    fs.readFileSync(path.join(repo, "packaging/clean-seed/nekorox/presets/spectrum-golden.json")))) {
  throw new Error("Shipped clean-seed spectrum preset differs from the canonical preset.");
}
const preset = JSON.parse(fs.readFileSync(
  path.join(repo, "config", "spectrum", "nekorox-golden.json"), "utf8"));
const forkLock = JSON.parse(fs.readFileSync(path.join(repo, "third_party",
  "spectrum-analyzer", "upstream.lock.json"), "utf8"));
if (preset.componentVersion !== forkLock.forkVersion) {
  throw new Error("Spectrum preset and pinned fork version diverged.");
}
if (preset.amplitude.minimumDb !== -80 ||
    preset.amplitude.maximumDb !== -10 ||
    preset.amplitude.legacyCalibrationDb !== -10) {
  throw new Error("Legacy spectrum amplitude calibration drifted.");
}
if (preset.peaks.mode !== 2 || preset.peaks.holdMs !== 0 ||
    preset.peaks.gravityDbPerSecondSquared !== 500) {
  throw new Error("Legacy spectrum peak ballistics drifted.");
}

const bands = preset.bands;
const noteFrequency = (note) => 440 * (2 ** ((note - 57) / 12));
if (bands.distribution !== "octaves" || bands.activeCount !== 120 ||
    bands.storedLinearCount !== 320 || bands.minimumHz !== 20 ||
    bands.maximumHz !== 20000 || bands.minimumNote !== 4 ||
    bands.maximumNote !== 123 || bands.perOctave !== 12 ||
    bands.activeCount !== bands.maximumNote - bands.minimumNote + 1 ||
    Math.abs(bands.minimumCenterHz - noteFrequency(bands.minimumNote)) > 1e-9 ||
    Math.abs(bands.maximumCenterHz - noteFrequency(bands.maximumNote)) > 1e-7) {
  throw new Error("Full-range musical spectrum distribution drifted.");
}

const renderer = preset.renderer;
if (renderer?.mode !== "double-exposure" ||
    renderer.horizontalAlignment !== "fit" ||
    renderer.shadow.attackMs !== 28 || renderer.shadow.releaseMs !== 460 ||
    renderer.shadow.opacity !== 0.72 ||
    renderer.body !== "legacy-neutral-ramp-with-artwork-accent-and-full-scale-white" ||
    renderer.paletteSource !== "shared-theme-snapshot-v2" ||
    renderer.paletteInterpolation !== "theme-service-oklch" ||
    renderer.shadow.palette !== "artwork-secondary-dark-ramp" ||
    renderer.shadow.maximumLuminance !== 0.17 ||
    renderer.memory !== false || renderer.peakArea !== false ||
    renderer.peakMarker.color !== "amplitude-curve-at-held-peak" ||
    renderer.peakMarker.heightSource !== "fb.MaxValue" ||
    renderer.peakMarker.independentFromCurrentBar !== true ||
    renderer.peakMarker.opacityFollowsDecay !== true ||
    renderer.fullScaleHighlight.start !== 0.75 ||
    renderer.fullScaleHighlight.full !== 1.0 ||
    renderer.fullScaleHighlight.maximumColor !== "#FFFFFF" ||
    renderer.fullScaleHighlight.pureWhiteOnlyAtFullScale !== true ||
    renderer.fullScaleHighlight.body !== true ||
    renderer.fullScaleHighlight.wholeBarSolidColor !== true ||
    renderer.fullScaleHighlight.peakUsesOwnHeight !== true ||
    renderer.grid.accented !== false ||
    renderer.surface.background !== "#080909" ||
    renderer.surface.darkBand !== "#0A0C0D" ||
    renderer.surface.lightBand !== "#0D1112" ||
    renderer.surface.artworkBackdrop !== false ||
    renderer.surface.purpleSeparators !== false ||
    preset.artworkPalette !== "shared-theme-snapshot-v2") {
  throw new Error("Double Exposure renderer contract drifted.");
}

const smoothStep = (value) => {
  const x = Math.max(0, Math.min(1, value));
  return x * x * (3 - 2 * x);
};
const impactWeight = (amplitude) => smoothStep(
  (amplitude - renderer.fullScaleHighlight.start) /
  (renderer.fullScaleHighlight.full - renderer.fullScaleHighlight.start));
if (impactWeight(0.75) !== 0 || Math.abs(impactWeight(0.875) - 0.5) > 1e-9 ||
    impactWeight(0.999) >= 1 || impactWeight(1.0) !== 1) {
  throw new Error("Pure white must remain exclusive to full-scale amplitude.");
}

const expectedLegacyGradient = [
  [0.0, "#151515"],
  [0.25, "#1F1F1F"],
  [0.50, "#2A2A2A"],
  [0.75, "artwork-accent"],
  [1.0, "#FFFFFF"],
];
for (const [index, [position, color]] of expectedLegacyGradient.entries()) {
  const stop = preset.barGradient[index];
  if (stop?.position !== position || stop?.color !== color) {
    throw new Error(`Legacy amplitude stop ${index} drifted.`);
  }
}
if (preset.barGradient[3].legacyColor !== "#008080" ||
    JSON.stringify(preset.legacyPalette?.highToLow) !== JSON.stringify(
      ["#FFFFFF", "#008080", "#2A2A2A", "#1F1F1F", "#151515"]) ||
    preset.legacyPalette?.dynamicSubstitution?.["#008080"] !== "artwork-accent") {
  throw new Error("Frozen x86 Musical Spectrum palette evidence drifted.");
}

const temporalHeader = fs.readFileSync(path.join(repo, "out", "third_party",
  "spectrum-analyzer", "foo_vis_spectrum_analyzer", "Visuals", "Spectrum",
  "NekoDoubleExposure.h"), "utf8");
const readSeconds = (name) => {
  const match = temporalHeader.match(new RegExp(
    `\\b${name}\\s*=\\s*([0-9.]+)`));
  if (!match) throw new Error(`Missing temporal constant ${name}.`);
  return Number(match[1]);
};
const temporalContract = {
  ShadowAttackSeconds: renderer.shadow.attackMs / 1000,
  ShadowReleaseSeconds: renderer.shadow.releaseMs / 1000,
};
for (const [name, expected] of Object.entries(temporalContract)) {
  if (Math.abs(readSeconds(name) - expected) > 1e-12) {
    throw new Error(`${name} no longer matches the golden preset.`);
  }
}

if (/MemoryAttackSeconds|MemoryReleaseSeconds|_Memory/.test(temporalHeader)) {
  throw new Error("Retired magenta memory layer returned to Double Exposure.");
}

const paletteHeader = fs.readFileSync(path.join(repo, "out", "third_party",
  "spectrum-analyzer", "foo_vis_spectrum_analyzer", "Visuals", "Spectrum",
  "NekoVisualPalette.h"), "utf8");
if (!paletteHeader.includes("LegacyDarkMid = 0.25") ||
    !paletteHeader.includes("LegacyDarkHigh = 0.50") ||
    !paletteHeader.includes("HighlightStart = 0.75") ||
    !paletteHeader.includes("HighlightFull = 1.00") ||
    !paletteHeader.includes("RefreshSharedTheme") ||
    !paletteHeader.includes("SharedThemeSnapshotV2") ||
    !paletteHeader.includes("AmplitudeColor") ||
    !paletteHeader.includes("ArtworkShadowColor") ||
    !paletteHeader.includes("ApplyNeutralGraphBackground") ||
    !paletteHeader.includes("0x080909") ||
    !paletteHeader.includes("D2D1::ColorF::White") ||
    !paletteHeader.includes("0x151515") ||
    !paletteHeader.includes("0x1F1F1F") ||
    !paletteHeader.includes("0x2A2A2A") ||
    paletteHeader.includes("0xB21B78") ||
    paletteHeader.includes("0x11020F")) {
  throw new Error("Artwork palette or full-scale highlight contract drifted.");
}
const amplitudeStart = paletteHeader.indexOf("inline D2D1_COLOR_F AmplitudeColor");
const amplitudeEnd = paletteHeader.indexOf("inline void SetSolidBrushColor");
const amplitudeBody = paletteHeader.slice(amplitudeStart, amplitudeEnd);
if (amplitudeStart < 0 || amplitudeEnd <= amplitudeStart ||
    amplitudeBody.includes("theme.secondary")) {
  throw new Error("Secondary artwork color leaked back into the live bar curve.");
}

const spectrumSource = fs.readFileSync(path.join(repo, "out", "third_party",
  "spectrum-analyzer", "foo_vis_spectrum_analyzer", "Visuals", "Spectrum",
  "Spectrum.cpp"), "utf8");
const graphSource = fs.readFileSync(path.join(repo, "out", "third_party",
  "spectrum-analyzer", "foo_vis_spectrum_analyzer", "Visuals",
  "Graph.cpp"), "utf8");
const providerHeader = fs.readFileSync(path.join(repo, "src", "component",
  "nekorox_theme_provider.h"), "utf8");
const buildSource = fs.readFileSync(path.join(repo, "tools",
  "build-spectrum-analyzer.ps1"), "utf8");
if (!spectrumSource.includes("RefreshSharedTheme(_NekoThemeProvider") ||
    !spectrumSource.includes("SpectrumColorWithCorrectionV2(_NekoTheme, value, correctionDb") ||
    !spectrumSource.includes("_BarPeakTopStyle, fb.MaxValue") ||
    !spectrumSource.includes("ArtworkShadowColor(") ||
    !graphSource.includes("ApplyNeutralGraphBackground") ||
    !providerHeader.includes("ThemeProviderV1") ||
    !providerHeader.includes("sizeof(SharedThemeSnapshotV2) == 144") ||
    !buildSource.includes("themeProviderHeaderSha256")) {
  throw new Error("Native shared-theme provider wiring drifted.");
}
if (!paletteHeader.includes("PerceptualCorrectionDb") ||
    !paletteHeader.includes("correctedDb + 50.") ||
    !paletteHeader.includes("correctedDb + 20.") ||
    !paletteHeader.includes("legacyHighlight") ||
    !paletteHeader.includes("HighlightStart") ||
    !paletteHeader.includes("20., 31.5, 63., 125., 250., 500., 1000.") ||
    !paletteHeader.includes("-12., -10., -6., -3., -1., 0., 0., 1.5, 2.5") ||
    !paletteHeader.includes("-3., -7., -12.")) {
  throw new Error("Goden Spectrum perceptual color curve V2 drifted.");
}
if (!spectrumSource.includes("const FLOAT yAxisReserve = (std::max)(yl, yr);") ||
    !spectrumSource.includes("_Rect.left + yAxisReserve, _Rect.top, _Rect.right - yAxisReserve") ||
    !spectrumSource.includes("_Rect.left + yAxisReserve - yl") ||
    !spectrumSource.includes("_Rect.right - yAxisReserve + yr") ||
    !spectrumSource.includes("_Rect.left + yAxisReserve + PaddingX") ||
    !spectrumSource.includes("_Rect.right - yAxisReserve - PaddingX")) {
  throw new Error("Spectrum side gutters are no longer symmetric around the plot.");
}
if (!spectrumSource.includes("constexpr FLOAT TargetBandPitch = 8.f") ||
    !spectrumSource.includes("_ClientSize.width / TargetBandPitch") ||
    !spectrumSource.includes("target.Value = std::max(target.Value, band.Value)") ||
    !spectrumSource.includes("target.Value = std::lerp(a.Value, b.Value, fraction)") ||
    !spectrumSource.includes("target.Center = mapping.center") ||
    !fs.readFileSync(path.join(repo, "third_party/spectrum-analyzer/NekoRenderSampling.h"), "utf8").includes("std::log(source[m.first].Center)") ||
    !spectrumSource.includes("const LONG bodyGapPixels") ||
    !spectrumSource.includes("nextPixels - bodyGapPixels") ||
    !spectrumSource.includes("GetBodyRect(renderIndex)") ||
    !spectrumSource.includes("const FLOAT BackgroundPitch = SpectrumWidth / (FLOAT) sourceBandCount") ||
    !spectrumSource.includes("_Analysis->_FrequencyBands.front().HasDarkBackground") ||
    spectrumSource.includes("target.HasDarkBackground") ||
    !buildSource.includes("0007-goden-adaptive-spectrum-sampling.patch") ||
    !buildSource.includes("0008-goden-independent-note-background.patch") ||
    !buildSource.includes("0009-goden-constant-physical-bar-gap.patch")) {
  throw new Error("Adaptive Spectrum sampling drifted from the 120-note source grid.");
}
const adaptiveBarMetrics = (clientWidth, dpi, offsetDip = 0) => {
  const renderCount = Math.max(12, Math.min(360,
    Math.round(clientWidth / 8)));
  const pitch = Math.max(clientWidth / renderCount, 2);
  const pixelScale = Math.max(dpi / 96, 0.25);
  const gapPixels = Math.max(1, Math.round(pixelScale));
  const boundary = (index) => Math.round(
    (offsetDip + pitch * index) * pixelScale);
  const widths = [];
  for (let index = 0; index < renderCount; ++index) {
    const left = boundary(index);
    const next = boundary(index + 1);
    const right = Math.max(left, next - gapPixels);
    widths.push(right - left);
    if (index + 1 < renderCount && boundary(index + 1) - right !== gapPixels) {
      throw new Error(`Spectrum gap doubled at bar ${index}/${clientWidth}px/${dpi}dpi.`);
    }
  }
  return { renderCount, pitch, gapPixels, pixelScale, widths };
};
for (const dpi of [96, 120, 144, 192, 240]) {
  for (const width of [500, 960, 1520]) {
    for (const offset of [0, 0.37, 3.25]) {
      const metrics = adaptiveBarMetrics(width, dpi, offset);
      const minimum = Math.min(...metrics.widths);
      const maximum = Math.max(...metrics.widths);
      if (metrics.pitch < 7.7 || metrics.pitch > 8.3 || minimum <= 0 ||
          maximum - minimum > 1) {
        throw new Error(`Invalid adaptive Spectrum metrics at ${width}px/${dpi}dpi.`);
      }
    }
  }
}
const compactBars = adaptiveBarMetrics(500, 96).renderCount;
const referenceBars = adaptiveBarMetrics(960, 96).renderCount;
const wideBars = adaptiveBarMetrics(1520, 96).renderCount;
if (!(compactBars < bands.activeCount &&
      referenceBars === bands.activeCount &&
      wideBars > bands.activeCount)) {
  throw new Error("Spectrum density no longer merges compact or subdivides wide panels.");
}

const approach = (current, target, dt, tau) =>
  current + ((target - current) * (1 - Math.exp(-dt / tau)));
const evolve = (refreshHz, duration, start, target, tau) => {
  const frameCount = Math.round(refreshHz * duration);
  let value = start;
  for (let i = 0; i < frameCount; ++i) {
    value = approach(value, target, duration / frameCount, tau);
  }
  return value;
};
for (const tau of Object.values(temporalContract)) {
  const at60 = evolve(60, 1, 1, 0, tau);
  const at165 = evolve(165, 1, 1, 0, tau);
  if (Math.abs(at60 - at165) > 1e-12) {
    throw new Error(`Temporal layer is refresh-rate dependent at tau=${tau}.`);
  }
}

const calibratedDb = (magnitude) =>
  20 * Math.log10(magnitude) + preset.amplitude.legacyCalibrationDb;
const normalized = (magnitude) => Math.max(0, Math.min(1,
  (calibratedDb(magnitude) - preset.amplitude.minimumDb) /
  (preset.amplitude.maximumDb - preset.amplitude.minimumDb)));
if (Math.abs(normalized(1) - 1) > 1e-12 || normalized(0.5) >= 1) {
  throw new Error("Legacy calibration no longer reserves full-scale headroom.");
}
const fallDb = (seconds) =>
  0.5 * preset.peaks.gravityDbPerSecondSquared * seconds * seconds;
if (Math.abs(fallDb(0.2) - 10) > 1e-9 ||
    Math.abs(fallDb(0.4) - 40) > 1e-9) {
  throw new Error("Peak gravity no longer matches the old 500 dB/s^2 curve.");
}

const fcl = parseFcl(fs.readFileSync(path.join(
  repo, "config", "layout", "nekorox-v3-cohesive-bottom.fcl")));
const dataset = fcl.datasets.find(
  (entry) => entry.guid === "2cf00365-f2d7-4e78-9fca-c1d56e2707d8");
if (!dataset) throw new Error("Columns UI layout dataset is missing.");
const layout = parseLayout(dataset.data, new Set([
  "77653a44-66d1-49e0-9a7a-1c71898c0441",
  "8fa0bc24-882a-4fff-8a3b-215ea7fbd07f",
]));
let spectrum = null;
function guidBytes(guid) {
  const match = guid.toLowerCase().match(
    /^([0-9a-f]{8})-([0-9a-f]{4})-([0-9a-f]{4})-([0-9a-f]{4})-([0-9a-f]{12})$/);
  if (!match) throw new Error(`Invalid GUID: ${guid}`);
  const output = Buffer.alloc(16);
  output.writeUInt32LE(parseInt(match[1], 16), 0);
  output.writeUInt16LE(parseInt(match[2], 16), 4);
  output.writeUInt16LE(parseInt(match[3], 16), 6);
  Buffer.from(`${match[4]}${match[5]}`, "hex").copy(output, 8);
  return output;
}
const shellGuid = "a3dc5b91-875e-4f0a-a743-7d3183e6f42c";
const spectrumGuid = "3247c894-e585-4025-a866-c7d4933fb2e3";
const spectrumGuidBytes = guidBytes(spectrumGuid);
function visit(panel) {
  if (panel.guid === spectrumGuid) {
    spectrum = panel;
  }
  // Since C3 the production layout stores the visual surfaces in the V3
  // PlayerShell payload instead of exposing them as splitter descendants.
  // Inspect that canonical container rather than silently losing coverage.
  if (panel.guid === shellGuid && panel.childData.length >= 16 &&
      panel.childData.readUInt32LE(0) === 3) {
    const count = panel.childData.readUInt32LE(12);
    let cursor = 16;
    for (let index = 0; index < count; ++index) {
      if (cursor + 20 > panel.childData.length) {
        throw new Error("Serialized PlayerShell table is truncated.");
      }
      const guid = panel.childData.subarray(cursor, cursor + 16);
      const length = panel.childData.readUInt32LE(cursor + 16);
      cursor += 20;
      if (cursor + length > panel.childData.length) {
        throw new Error("Serialized PlayerShell child is truncated.");
      }
      if (guid.equals(spectrumGuidBytes)) {
        spectrum = { childData: panel.childData.subarray(cursor, cursor + length) };
      }
      cursor += length;
    }
    if (cursor !== panel.childData.length) {
      throw new Error("Serialized PlayerShell table has trailing bytes.");
    }
  }
  for (const child of panel.children?.children ?? []) visit(child);
}
for (const layoutPreset of layout.presets) visit(layoutPreset);
if (!spectrum || spectrum.childData.readInt32LE(93) !== 1 ||
    spectrum.childData.readDoubleLE(105) !== bands.minimumHz ||
    spectrum.childData.readDoubleLE(113) !== bands.maximumHz ||
    spectrum.childData.readUInt32LE(121) !== bands.minimumNote ||
    spectrum.childData.readUInt32LE(125) !== bands.maximumNote ||
    spectrum.childData.readUInt32LE(129) !== bands.perOctave ||
    spectrum.childData.readUInt32LE(4956) !== 4 ||
    Number(spectrum.childData.readBigUInt64LE(4961)) !== 1 ||
    spectrum.childData.readInt32LE(5056) !== 3 ||
    spectrum.childData.readInt32LE(5060) !== 1 ||
    spectrum.childData.readInt32LE(219) !== 2 ||
    spectrum.childData.readDoubleLE(223) !== 0 ||
    spectrum.childData.readDoubleLE(231) !== 0.5) {
  throw new Error("Serialized Spectrum peak response does not match the preset.");
}

const buildScript = fs.readFileSync(path.join(
  repo, "tools", "build-spectrum-analyzer.ps1"), "utf8");
if (!buildScript.includes("0011-goden-stop-release.patch")) {
  throw new Error("Stop-release patch is not in the canonical Spectrum build.");
}
const stopPatch = fs.readFileSync(path.join(
  repo, "third_party", "spectrum-analyzer", "patches",
  "0011-goden-stop-release.patch"), "utf8");
if (!stopPatch.includes("_PauseReleaseActive = stopped") ||
    !stopPatch.includes("if (!stopped)")) {
  throw new Error("Stable Stop no longer owns a bounded release-to-silence.");
}
if (!buildScript.includes("0012-goden-coalesced-stop-release.patch")) {
  throw new Error(
    "Coalesced Stop-release patch is not in the canonical Spectrum build.");
}
const coalescedStopPatch = fs.readFileSync(path.join(
  repo, "third_party", "spectrum-analyzer", "patches",
  "0012-goden-coalesced-stop-release.patch"), "utf8");
if (!coalescedStopPatch.includes(
      "_PlaybackStopped.load(std::memory_order_acquire)") ||
    !coalescedStopPatch.includes(
      "const bool startingTrack = !stopped")) {
  throw new Error(
    "Authoritative stopped state no longer wins coalesced transport events.");
}
if (!buildScript.includes("0013-goden-release-deadline.patch")) {
  throw new Error(
    "Release deadline patch is not in the canonical Spectrum build.");
}
const releaseDeadlinePatch = fs.readFileSync(path.join(
  repo, "third_party", "spectrum-analyzer", "patches",
  "0013-goden-release-deadline.patch"), "utf8");
if (!releaseDeadlinePatch.includes("GetTickCount64() + 1500ULL") ||
    !releaseDeadlinePatch.includes("_PauseReleaseFinalFrame = true") ||
    !releaseDeadlinePatch.includes("!active || deadlineReached")) {
  throw new Error(
    "Pause/Stop release no longer has a bounded final clean frame.");
}
if (!buildScript.includes("0014-goden-portable-preset-path.patch")) {
  throw new Error(
    "Portable Spectrum preset path patch is not in the canonical build.");
}
const portablePathPatch = fs.readFileSync(path.join(
  repo, "third_party", "spectrum-analyzer", "patches",
  "0014-goden-portable-preset-path.patch"), "utf8");
if (!portablePathPatch.includes("Path.is_empty()") ||
    !portablePathPatch.includes("never persist a") ||
    !portablePathPatch.includes('STR_RELEASE_TAG         "-goden6"')) {
  throw new Error(
    "Spectrum preset roots are no longer serialized portably.");
}

console.log("NekoRoX spectrum response + Double Exposure renderer: PASS");
