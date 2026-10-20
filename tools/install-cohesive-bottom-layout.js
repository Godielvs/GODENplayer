"use strict";

const fs = require("fs");
const path = require("path");
const {
  buildFcl,
  buildLayout,
  makePanel,
  parseFcl,
  parseLayout,
  pruneUnreferencedPanels,
} = require("./fcl-codec");

const GUID = {
  column: "77653a44-66d1-49e0-9a7a-1c71898c0441",
  row: "8fa0bc24-882a-4fff-8a3b-215ea7fbd07f",
  waveSeek: "369b09d4-58a7-4730-a2ce-ddd824e83213",
  transport: "37dfaaa1-5040-4966-819f-d012f41a6078",
  spectrum: "3247c894-e585-4025-a866-c7d4933fb2e3",
  cover: "5d0cc69e-d3d0-4c82-90db-40c7cf9c3c9e",
  caption: "6d9a108b-cc44-44ba-8854-5f7bd0cd3310",
  shell: "a3dc5b91-875e-4f0a-a743-7d3183e6f42c",
  layoutDataset: "2cf00365-f2d7-4e78-9fca-c1d56e2707d8",
};

const [, , inputArgument, outputArgument] = process.argv;
if (!inputArgument || !outputArgument) {
  throw new Error(
    "Usage: node install-cohesive-bottom-layout.js <input.fcl> <output.fcl>");
}

const input = path.resolve(inputArgument);
const output = path.resolve(outputArgument);
const splitterGuids = new Set([GUID.column, GUID.row]);
const fcl = parseFcl(fs.readFileSync(input));
const dataset = fcl.datasets.find(
  (entry) => entry.guid === GUID.layoutDataset);
if (!dataset) throw new Error("Columns UI layout dataset was not found.");

function registerPanel(guid, name) {
  let index = fcl.panels.findIndex((panel) => panel.guid === guid);
  if (index < 0) {
    index = fcl.panels.length;
    fcl.panels.push({ guid, name });
  }
  return index;
}

const captionIndex = registerPanel(
  GUID.caption,
  "NekoRoX Track Caption");
const columnIndex = fcl.panels.findIndex(
  (panel) => panel.guid === GUID.column);
if (columnIndex < 0) throw new Error("Columns UI Column declaration is missing.");

const layout = parseLayout(dataset.data, splitterGuids);
const preset = layout.presets[layout.active] ?? layout.presets[0];
const root = preset?.children?.children;
if (!root || preset.guid !== GUID.column) {
  throw new Error(
    "Active Columns UI preset is not the expected vertical root splitter.");
}

const shell = root.find((panel) => panel.guid === GUID.shell);
if (shell && shell.childData.length >= 4 &&
    (shell.childData.readUInt32LE(0) === 2 ||
      shell.childData.readUInt32LE(0) === 3)) {
  fs.mkdirSync(path.dirname(output), { recursive: true });
  fs.copyFileSync(input, output);
  console.log(JSON.stringify({
    schema: "goden.player-shell-layout.v3",
    input,
    output,
    action: "already-shell-noop",
  }, null, 2));
  process.exit(0);
}

const waveSeekPosition = root.findIndex(
  (panel) => panel.guid === GUID.waveSeek);
if (waveSeekPosition < 1 ||
    root[waveSeekPosition - 1]?.guid !== GUID.transport) {
  throw new Error("WaveSeek is not directly below NekoRoX Transport.");
}
const bottom = root[waveSeekPosition + 1];
if (!bottom || bottom.guid !== GUID.row || !bottom.children) {
  throw new Error("The expected spectrum/cover row does not follow WaveSeek.");
}

const spectrum = bottom.children.children.find(
  (panel) => panel.guid === GUID.spectrum);
const cover = bottom.children.children.find(
  (panel) => panel.guid === GUID.cover);
if (!spectrum || !cover || bottom.children.children.length !== 2) {
  throw new Error("The existing bottom row is not the validated spectrum/cover pair.");
}

const waveSeek = root[waveSeekPosition];
const waveSeekHeight = 36;
const captionHeight = 22;
const combinedHeight = waveSeek.size + bottom.size;
const spectrumHeight = combinedHeight - waveSeekHeight - captionHeight;
const spectrumWidth = spectrum.size;
const spectrumWidthDpi = spectrum.extraDpi ?? 96;
if (spectrumHeight < 120) {
  throw new Error(`Refusing implausible spectrum height: ${spectrumHeight}`);
}

// Columns UI proportionally shrinks unlocked panes.  The source layout was
// authored on a wide window (1285 + 220), so an unlocked cover collapsed to
// roughly 100 px on a 740 px window.  Keep the artwork rail at its authored
// DPI-aware width; the Cover panel itself already fits a square to the
// available height and requests an exact physical-pixel artwork variant.
cover.size = 220;
cover.extraSize = 220;
cover.extraDpi = 96;
cover.locked = true;

waveSeek.size = waveSeekHeight;
waveSeek.extraSize = waveSeekHeight;
spectrum.size = spectrumHeight;
spectrum.extraSize = spectrumHeight;
const caption = makePanel(GUID.caption, captionHeight, 96);
const leftColumn = makePanel(
  GUID.column,
  spectrumWidth,
  spectrumWidthDpi);
leftColumn.children = {
  version: 0,
  children: [waveSeek, spectrum, caption],
};

bottom.size = combinedHeight;
bottom.extraSize = combinedHeight;
bottom.children.children = [leftColumn, cover];
root.splice(waveSeekPosition, 2, bottom);

if (!dataset.required.includes(captionIndex)) {
  dataset.required.push(captionIndex);
}
// A new nested splitter is now part of the serialized layout graph.
dataset.required.push(columnIndex);
dataset.data = buildLayout(layout, splitterGuids);
const removedPanels = pruneUnreferencedPanels(fcl);

fs.mkdirSync(path.dirname(output), { recursive: true });
fs.writeFileSync(output, buildFcl(fcl));
console.log(JSON.stringify({
  schema: "nekorox.cohesive-bottom-layout.v1",
  input,
  output,
  combinedHeight,
  waveSeekHeight: waveSeek.size,
  spectrumHeight,
  captionHeight,
  coverWidth: cover.size,
  coverLocked: cover.locked,
  removedPanels,
}, null, 2));
