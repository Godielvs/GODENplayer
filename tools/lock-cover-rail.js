"use strict";

const fs = require("fs");
const path = require("path");
const { buildFcl, buildLayout, parseFcl, parseLayout } = require("./fcl-codec");

const LAYOUT_DATASET = "2cf00365-f2d7-4e78-9fca-c1d56e2707d8";
const COLUMN = "77653a44-66d1-49e0-9a7a-1c71898c0441";
const ROW = "8fa0bc24-882a-4fff-8a3b-215ea7fbd07f";
const COVER = "5d0cc69e-d3d0-4c82-90db-40c7cf9c3c9e";
const [, , inputArgument] = process.argv;
if (!inputArgument) {
  throw new Error("Usage: node lock-cover-rail.js <layout.fcl>");
}

const file = path.resolve(inputArgument);
const fcl = parseFcl(fs.readFileSync(file));
const dataset = fcl.datasets.find((entry) => entry.guid === LAYOUT_DATASET);
if (!dataset) throw new Error("Columns UI layout dataset is missing.");
const splitters = new Set([COLUMN, ROW]);
const layout = parseLayout(dataset.data, splitters);
let matches = 0;
for (const preset of layout.presets) {
  const visit = (panels) => {
    for (const panel of panels) {
      if (panel.guid === COVER) {
        panel.size = 220;
        panel.locked = true;
        matches += 1;
      }
      if (panel.children) visit(panel.children.children);
    }
  };
  if (preset.children) visit(preset.children.children);
}
if (matches !== 1) {
  throw new Error(`Expected one Cover panel across presets; found ${matches}.`);
}
dataset.data = buildLayout(layout, splitters);
fs.writeFileSync(file, buildFcl(fcl));
console.log(`Locked GODENplayer Cover rail at 220 DIP: ${file}`);
