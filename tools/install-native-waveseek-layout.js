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
  waveformMinibar: "8b15938a-a993-4101-8441-f84fe0532846",
  waveSeek: "369b09d4-58a7-4730-a2ce-ddd824e83213",
  transport: "37dfaaa1-5040-4966-819f-d012f41a6078",
  layoutDataset: "2cf00365-f2d7-4e78-9fca-c1d56e2707d8",
};

const [, , inputArgument, outputArgument] = process.argv;
if (!inputArgument || !outputArgument) {
  throw new Error(
    "Usage: node install-native-waveseek-layout.js <input.fcl> <output.fcl>");
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

function replacementFrom(existing, guid) {
  const replacement = makePanel(
    guid,
    existing.size,
    existing.extraDpi ?? 96);
  replacement.captionOrientation = existing.captionOrientation;
  replacement.locked = existing.locked;
  replacement.hidden = existing.hidden;
  replacement.showCaption = existing.showCaption;
  replacement.autohide = existing.autohide;
  replacement.showToggleArea = existing.showToggleArea;
  return replacement;
}

const waveSeekIndex = registerPanel(
  GUID.waveSeek,
  "NekoRoX WaveSeek");
const layout = parseLayout(dataset.data, splitterGuids);
const preset = layout.presets[layout.active] ?? layout.presets[0];
const root = preset?.children?.children;
if (!root || preset.guid !== GUID.column) {
  throw new Error(
    "Active Columns UI preset is not the expected vertical root splitter.");
}

let replacedCount = 0;
function replaceWaveform(panel) {
  let current = panel;
  if (current.guid === GUID.waveformMinibar) {
    current = replacementFrom(current, GUID.waveSeek);
    replacedCount += 1;
  }
  if (current.children) {
    current.children.children = current.children.children.map(
      replaceWaveform);
  }
  return current;
}
layout.presets = layout.presets.map(replaceWaveform);

const resolvedPreset =
  layout.presets[layout.active] ?? layout.presets[0];
const resolvedRoot = resolvedPreset?.children?.children;
const position = resolvedRoot?.findIndex(
  (panel) => panel.guid === GUID.waveSeek) ?? -1;
if (position < 0) {
  throw new Error(
    "Neither Waveform Minibar nor NekoRoX WaveSeek was found.");
}
if (position < 1 ||
    resolvedRoot[position - 1].guid !== GUID.transport) {
  throw new Error(
    "WaveSeek is not directly below NekoRoX Transport.");
}
if (resolvedRoot[position].size < 18 ||
    resolvedRoot[position].size > 80) {
  throw new Error(
    `Refusing implausible WaveSeek height: ${resolvedRoot[position].size}`);
}

const minibarIndex = fcl.panels.findIndex(
  (panel) => panel.guid === GUID.waveformMinibar);
if (minibarIndex >= 0) {
  dataset.required = dataset.required.map(
    (index) => index === minibarIndex ? waveSeekIndex : index);
} else if (!dataset.required.includes(waveSeekIndex)) {
  dataset.required.push(waveSeekIndex);
}
dataset.data = buildLayout(layout, splitterGuids);
const removedPanels = pruneUnreferencedPanels(fcl);
fs.mkdirSync(path.dirname(output), { recursive: true });
fs.writeFileSync(output, buildFcl(fcl));

console.log(JSON.stringify({
  schema: "nekorox.waveseek-layout.v1",
  input,
  output,
  waveSeekIndex,
  waveSeekSize: resolvedRoot[position].size,
  replacedCount,
  removedPanels,
}, null, 2));
