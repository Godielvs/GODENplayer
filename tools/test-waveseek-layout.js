"use strict";

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");
const { parseFcl, parseLayout } = require("./fcl-codec");

const GUID = {
  column: "77653a44-66d1-49e0-9a7a-1c71898c0441",
  row: "8fa0bc24-882a-4fff-8a3b-215ea7fbd07f",
  waveformMinibar: "8b15938a-a993-4101-8441-f84fe0532846",
  waveSeek: "369b09d4-58a7-4730-a2ce-ddd824e83213",
  transport: "37dfaaa1-5040-4966-819f-d012f41a6078",
  spectrum: "3247c894-e585-4025-a866-c7d4933fb2e3",
  cover: "5d0cc69e-d3d0-4c82-90db-40c7cf9c3c9e",
  layoutDataset: "2cf00365-f2d7-4e78-9fca-c1d56e2707d8",
};

const cacheSource = fs.readFileSync(path.join(
  __dirname, "..", "src", "component", "waveform_cache_service.cpp"), "utf8");
if (!/kRequestDwell\s*=\s*std::chrono::milliseconds\(12\)/.test(cacheSource) ||
    !/kProgressPublishInterval\s*=\s*std::chrono::milliseconds\(33\)/.test(cacheSource) ||
    !/kProgressPublishMinimumBins\s*=\s*24/.test(cacheSource) ||
    !/kInitialPreviewMinimumBins\s*=\s*8/.test(cacheSource)) {
  throw new Error("WaveSeek low-latency progressive publication drifted.");
}
const previewPublish = cacheSource.indexOf(
  "Persistence is an optimization, not a gate for first paint");
const persistentLoad = cacheSource.indexOf("persistenceHook->load(");
if (previewPublish < 0 || persistentLoad < 0 ||
    previewPublish > persistentLoad) {
  throw new Error("WaveSeek persistence returned to the first-paint path.");
}

const panelSource = fs.readFileSync(path.join(
  __dirname, "..", "src", "component", "wave_seek_panel.cpp"), "utf8");
if (!/kWaveSeekReconstructTiles\s*=\s*36/.test(panelSource) ||
    !panelSource.includes("GlyphFlowOverlayCell") ||
    !panelSource.includes("waveGlyphRenderer_.drawOverlay") ||
    !panelSource.includes("kWaveSeekCoverageMotion") ||
    !panelSource.includes("motion_.retarget(") ||
    !panelSource.includes("presentedCoverage") ||
    !panelSource.includes("geometryCoverage_") ||
    !panelSource.includes("const auto knownClip") ||
    !panelSource.includes("const auto unknownClip") ||
    !panelSource.includes("never needs a second whole-lane swap") ||
    !panelSource.includes("const auto expectedBins") ||
    !panelSource.includes("waveformCalibrationFrozen_") ||
    !panelSource.includes("cannot renormalize material the user has seen") ||
    !panelSource.includes("geometryColumnCount_") ||
    !panelSource.includes("leave columns beyond the decoded prefix transparent") ||
    panelSource.includes("normalizedX * waveformColumns_.size()") ||
    !panelSource.includes("if (generationChanged)") ||
    !panelSource.includes("kWaveSeekStateMessageV1") ||
    !panelSource.includes("outgoingRmsGeometry_") ||
    panelSource.includes("drawReconstructionGlyph") ||
    panelSource.includes("generationChanged || completedThisUpdate") ||
    panelSource.includes("const auto local = materialComplete") ||
    /DrawGeometry\(\s*peakGeometry_/.test(panelSource)) {
  throw new Error("WaveSeek layered glyph reconstruction contract drifted.");
}

const [, , inputArgument] = process.argv;
if (!inputArgument) {
  throw new Error("Usage: node test-waveseek-layout.js <candidate.fcl>");
}

const candidate = path.resolve(inputArgument);
const fcl = parseFcl(fs.readFileSync(candidate));
const dataset = fcl.datasets.find(
  (entry) => entry.guid === GUID.layoutDataset);
if (!dataset) throw new Error("Columns UI layout dataset is missing.");
if (fcl.panels.some((panel) => panel.guid === GUID.waveformMinibar)) {
  throw new Error("Waveform Minibar remains in the candidate dependency table.");
}
if (!fcl.panels.some((panel) => panel.guid === GUID.waveSeek)) {
  throw new Error("NekoRoX WaveSeek is missing from the dependency table.");
}
const waveSeekIndex = fcl.panels.findIndex(
  (panel) => panel.guid === GUID.waveSeek);
const waveSeekRequirements = dataset.required.filter(
  (index) => index === waveSeekIndex).length;
if (dataset.required.length !== 17 || waveSeekRequirements !== 2) {
  throw new Error(
    `WaveSeek dependency cardinality drifted: ${dataset.required.length} total, ` +
    `${waveSeekRequirements} WaveSeek references.`);
}

const layout = parseLayout(
  dataset.data,
  new Set([GUID.column, GUID.row]));
const preset = layout.presets[layout.active] ?? layout.presets[0];
const root = preset?.children?.children;
if (!root) throw new Error("Active layout has no root children.");
const position = root.findIndex(
  (panel) => panel.guid === GUID.waveSeek);
if (position < 1 || root[position - 1].guid !== GUID.transport) {
  throw new Error("WaveSeek is not directly below NekoRoX Transport.");
}
if (root.filter((panel) => panel.guid === GUID.waveSeek).length !== 1) {
  throw new Error("Candidate layout contains duplicate WaveSeek panels.");
}
if (root[position].size !== 36) {
  throw new Error(`WaveSeek fixed height drifted: ${root[position].size}.`);
}

function findPanel(panels, guid) {
  for (const panel of panels) {
    if (panel.guid === guid) return panel;
    const nested = findPanel(panel.children?.children ?? [], guid);
    if (nested) return nested;
  }
  return null;
}

const spectrum = findPanel(root, GUID.spectrum);
if (!spectrum) {
  throw new Error("Spectrum Analyzer is missing from the candidate layout.");
}
const cover = findPanel(root, GUID.cover);
if (!cover || cover.size !== 220 || !cover.locked) {
  throw new Error(
    `Cover rail must stay locked at 220 DIP; got size=${cover?.size} ` +
      `locked=${cover?.locked}.`,
  );
}
const spectrumHash = crypto
  .createHash("sha256")
  .update(spectrum.childData)
  .digest("hex");
// Sanitized fixture: identical Spectrum settings with private preset path removed.
const expectedSpectrumHash =
  "3a754c68d505562e2a6d3fee53486ee67be7b0898a0d37ebb35130735eb8c40c";
if (spectrumHash !== expectedSpectrumHash) {
  throw new Error(`Spectrum payload drifted: ${spectrumHash}`);
}

console.log("NekoRoX WaveSeek candidate layout: PASS");
