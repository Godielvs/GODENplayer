"use strict";

const fs = require("fs");
const path = require("path");
const { buildFcl, buildLayout, makePanel, parseFcl, parseLayout } = require("./fcl-codec");

const GUID = {
  column: "77653a44-66d1-49e0-9a7a-1c71898c0441",
  row: "8fa0bc24-882a-4fff-8a3b-215ea7fbd07f",
  jscript: "0182f8a8-33a9-4486-b77e-938c7a08954a",
  waveform: "8b15938a-a993-4101-8441-f84fe0532846",
  workspace: "6c52b18d-4664-4e8f-8dcd-ce2b82b76b32",
  layoutDataset: "2cf00365-f2d7-4e78-9fca-c1d56e2707d8",
};

const [, , inputArgument, outputArgument] = process.argv;
if (!inputArgument || !outputArgument) {
  throw new Error("Usage: node install-workspace-layout.js <input.fcl> <output.fcl>");
}

const input = path.resolve(inputArgument);
const output = path.resolve(outputArgument);
const splitterGuids = new Set([GUID.column, GUID.row]);
const fcl = parseFcl(fs.readFileSync(input));
const layoutDataset = fcl.datasets.find((dataset) => dataset.guid === GUID.layoutDataset);
if (!layoutDataset) throw new Error("Columns UI layout dataset was not found.");

let workspaceIndex = fcl.panels.findIndex((panel) => panel.guid === GUID.workspace);
if (workspaceIndex < 0) {
  workspaceIndex = fcl.panels.length;
  fcl.panels.push({ guid: GUID.workspace, name: "NekoRoX Workspace" });
}
if (!layoutDataset.required.includes(workspaceIndex)) {
  layoutDataset.required.push(workspaceIndex);
}

const layout = parseLayout(layoutDataset.data, splitterGuids);
const preset = layout.presets[layout.active] ?? layout.presets[0];
if (!preset?.children || preset.guid !== GUID.column) {
  throw new Error("Active Columns UI preset is not the expected vertical root splitter.");
}

const children = preset.children.children;
const waveformIndex = children.findIndex((panel) => panel.guid === GUID.waveform);
if (waveformIndex < 3) throw new Error("Could not locate the current main workspace above the waveform.");
const transportIndex = waveformIndex - 1;
const mainIndex = waveformIndex - 2;
const tabsIndex = waveformIndex - 3;
if (children[tabsIndex].guid !== GUID.jscript ||
    children[mainIndex].guid !== GUID.row ||
    children[transportIndex].guid !== GUID.jscript) {
  throw new Error("Current layout does not match the frozen V3 baseline shape.");
}

const workspaceSize = children[tabsIndex].size + children[mainIndex].size;
const dpi = children[mainIndex].extraDpi ?? children[tabsIndex].extraDpi ?? 96;
children.splice(tabsIndex, 2, makePanel(GUID.workspace, workspaceSize, dpi));
layoutDataset.data = buildLayout(layout, splitterGuids);

fs.mkdirSync(path.dirname(output), { recursive: true });
fs.writeFileSync(output, buildFcl(fcl));
console.log(JSON.stringify({
  input,
  output,
  workspaceIndex,
  workspaceSize,
  rootChildren: children.map((panel) => panel.guid),
}, null, 2));
