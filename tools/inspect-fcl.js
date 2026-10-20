"use strict";

const fs = require("fs");
const path = require("path");
const crypto = require("crypto");
const { parseFcl, parseLayout } = require("./fcl-codec");

const GUID = {
  column: "77653a44-66d1-49e0-9a7a-1c71898c0441",
  row: "8fa0bc24-882a-4fff-8a3b-215ea7fbd07f",
  layoutDataset: "2cf00365-f2d7-4e78-9fca-c1d56e2707d8",
};

const [, , inputArgument] = process.argv;
if (!inputArgument) throw new Error("Usage: node inspect-fcl.js <input.fcl>");

const input = path.resolve(inputArgument);
const splitterGuids = new Set([GUID.column, GUID.row]);
const fcl = parseFcl(fs.readFileSync(input));
const names = new Map(fcl.panels.map((panel) => [panel.guid, panel.name]));
const dataset = fcl.datasets.find((item) => item.guid === GUID.layoutDataset);
if (!dataset) throw new Error("Columns UI layout dataset was not found.");
const layout = parseLayout(dataset.data, splitterGuids);

function describe(panel) {
  const result = {
    guid: panel.guid,
    name: names.get(panel.guid) ?? "Unknown",
    size: panel.size,
    locked: panel.locked,
    hidden: panel.hidden,
    showCaption: panel.showCaption,
    childConfigBytes: panel.childData?.length ?? 0,
    childConfigSha256: panel.childData
      ? crypto.createHash("sha256").update(panel.childData).digest("hex")
      : null,
  };
  if (panel.children) {
    result.children = panel.children.children.map(describe);
  }
  return result;
}

console.log(JSON.stringify({
  input,
  activePreset: layout.active,
  presets: layout.presets.map((preset, index) => ({
    index,
    name: preset.name,
    guid: preset.guid,
    children: preset.children ? preset.children.children.map(describe) : [],
  })),
}, null, 2));
