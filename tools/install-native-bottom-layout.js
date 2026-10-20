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
  jscript: "0182f8a8-33a9-4486-b77e-938c7a08954a",
  waveform: "8b15938a-a993-4101-8441-f84fe0532846",
  spectrum: "3247c894-e585-4025-a866-c7d4933fb2e3",
  cover: "5d0cc69e-d3d0-4c82-90db-40c7cf9c3c9e",
  layoutDataset: "2cf00365-f2d7-4e78-9fca-c1d56e2707d8",
};

const [, , inputArgument, outputArgument] = process.argv;
if (!inputArgument || !outputArgument) {
  throw new Error("Usage: node install-native-bottom-layout.js <input.fcl> <output.fcl>");
}

const input = path.resolve(inputArgument);
const output = path.resolve(outputArgument);
const splitterGuids = new Set([GUID.column, GUID.row]);
const fcl = parseFcl(fs.readFileSync(input));
const layoutDataset = fcl.datasets.find((dataset) => dataset.guid === GUID.layoutDataset);
if (!layoutDataset) throw new Error("Columns UI layout dataset was not found.");

function registerPanel(guid, name) {
  let index = fcl.panels.findIndex((panel) => panel.guid === guid);
  if (index < 0) {
    index = fcl.panels.length;
    fcl.panels.push({ guid, name });
  }
  if (!layoutDataset.required.includes(index)) layoutDataset.required.push(index);
  return index;
}

function nativePanelFrom(existing, guid) {
  const replacement = makePanel(guid, existing.size, existing.extraDpi ?? 96);
  replacement.captionOrientation = existing.captionOrientation;
  replacement.locked = existing.locked;
  replacement.hidden = existing.hidden;
  replacement.showCaption = existing.showCaption;
  replacement.autohide = existing.autohide;
  replacement.showToggleArea = existing.showToggleArea;
  return replacement;
}

function panelTreeUses(panel, guid) {
  if (panel.guid === guid) return true;
  return panel.children?.children.some((child) => panelTreeUses(child, guid)) ?? false;
}

const coverIndex = registerPanel(GUID.cover, "NekoRoX Cover");
const layout = parseLayout(layoutDataset.data, splitterGuids);
const preset = layout.presets[layout.active] ?? layout.presets[0];
if (!preset?.children || preset.guid !== GUID.column) {
  throw new Error("Active Columns UI preset is not the expected vertical root splitter.");
}

const root = preset.children.children;
const waveformPosition = root.findIndex((panel) => panel.guid === GUID.waveform);
if (waveformPosition < 0 || waveformPosition + 1 >= root.length) {
  throw new Error("Waveform and bottom row were not found in the expected order.");
}
const bottom = root[waveformPosition + 1];
if (bottom.guid !== GUID.row || !bottom.children || bottom.children.children.length !== 2) {
  throw new Error("Bottom band is not the expected two-panel row splitter.");
}
const children = bottom.children.children;
if (children[0].guid !== GUID.spectrum) {
  throw new Error("Refusing to alter a bottom row whose first panel is not Spectrum Analyzer.");
}
const originalSpectrumConfig = Buffer.from(children[0].childData);
if (children[1].guid === GUID.jscript) {
  children[1] = nativePanelFrom(children[1], GUID.cover);
} else if (children[1].guid !== GUID.cover) {
  throw new Error("Bottom-right panel is neither the frozen JScript cover nor NekoRoX Cover.");
}
children[1].size = 220;
if (!Buffer.from(children[0].childData).equals(originalSpectrumConfig)) {
  throw new Error("Spectrum Analyzer payload changed during the bottom layout transform.");
}

const jscriptIndex = fcl.panels.findIndex((panel) => panel.guid === GUID.jscript);
const jscriptStillUsed = layout.presets.some((candidate) =>
  candidate.children?.children.some((panel) => panelTreeUses(panel, GUID.jscript)) ?? false);
if (!jscriptStillUsed && jscriptIndex >= 0) {
  layoutDataset.required = layoutDataset.required.filter((index) => index !== jscriptIndex);
}
if (!layoutDataset.required.includes(coverIndex)) layoutDataset.required.push(coverIndex);

layoutDataset.data = buildLayout(layout, splitterGuids);
const removedPanels = pruneUnreferencedPanels(fcl);
fs.mkdirSync(path.dirname(output), { recursive: true });
fs.writeFileSync(output, buildFcl(fcl));

console.log(JSON.stringify({
  input,
  output,
  spectrumBytes: children[0].childData.length,
  coverSize: children[1].size,
  jscriptStillUsed,
  removedPanels,
}, null, 2));
