"use strict";

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");
const {
  buildFcl,
  buildLayout,
  parseFcl,
  parseLayout,
} = require("./fcl-codec");

const GUID = {
  column: "77653a44-66d1-49e0-9a7a-1c71898c0441",
  row: "8fa0bc24-882a-4fff-8a3b-215ea7fbd07f",
  spectrum: "3247c894-e585-4025-a866-c7d4933fb2e3",
  layoutDataset: "2cf00365-f2d7-4e78-9fca-c1d56e2707d8",
};

const SPECTRUM = {
  distributionOffset: 93,
  loFrequencyOffset: 105,
  hiFrequencyOffset: 113,
  minNoteOffset: 121,
  maxNoteOffset: 125,
  bandsPerOctaveOffset: 129,
  octaveDistribution: 1,
  minimumHz: 20,
  maximumHz: 20000,
  minimumNote: 4,   // E0, 20.60 Hz; its lower half-step edge is about 20 Hz.
  maximumNote: 123, // D#10, 19.91 kHz.
  bandsPerOctave: 12,
  graphDescriptionVersionOffset: 4956,
  graphCountOffset: 4961,
  horizontalAlignmentOffset: 5056,
  verticalAlignmentOffset: 5060,
  graphDescriptionVersion: 4,
  graphCount: 1,
  centerAlignment: 1,
  fitAlignment: 3,
};

const [, , inputArgument, outputArgument] = process.argv;
if (!inputArgument || !outputArgument) {
  throw new Error(
    "Usage: node tune-spectrum-legacy-response.js <input.fcl> <output.fcl>");
}

const input = path.resolve(inputArgument);
const output = path.resolve(outputArgument);
const splitterGuids = new Set([GUID.column, GUID.row]);
const fcl = parseFcl(fs.readFileSync(input));
const dataset = fcl.datasets.find(
  (entry) => entry.guid === GUID.layoutDataset);
if (!dataset) throw new Error("Columns UI layout dataset is missing.");

const layout = parseLayout(dataset.data, splitterGuids);
let tuned = 0;

function nearlyEqual(left, right) {
  return Math.abs(left - right) <= 1e-9;
}

function tunePanel(panel) {
  if (panel.guid === GUID.spectrum) {
    const data = panel.childData;
    if (!Buffer.isBuffer(data) || data.length !== 5216) {
      throw new Error(`Unexpected Spectrum payload size: ${data?.length}.`);
    }
    if (Number(data.readBigUInt64LE(0)) !== 35 ||
        !nearlyEqual(data.readDoubleLE(189), -80) ||
        !nearlyEqual(data.readDoubleLE(197), -10) ||
        !nearlyEqual(data.readDoubleLE(4991), -80) ||
        !nearlyEqual(data.readDoubleLE(4999), -10)) {
      throw new Error("Spectrum payload schema or amplitude range drifted.");
    }
    if (data.readInt32LE(SPECTRUM.distributionOffset) !==
          SPECTRUM.octaveDistribution ||
        !nearlyEqual(data.readDoubleLE(SPECTRUM.loFrequencyOffset),
          SPECTRUM.minimumHz) ||
        !nearlyEqual(data.readDoubleLE(SPECTRUM.hiFrequencyOffset),
          SPECTRUM.maximumHz) ||
        data.readUInt32LE(SPECTRUM.bandsPerOctaveOffset) !==
          SPECTRUM.bandsPerOctave) {
      throw new Error("Spectrum musical frequency distribution drifted.");
    }
    const horizontalAlignment = data.readInt32LE(
      SPECTRUM.horizontalAlignmentOffset);
    if (data.readUInt32LE(SPECTRUM.graphDescriptionVersionOffset) !==
          SPECTRUM.graphDescriptionVersion ||
        Number(data.readBigUInt64LE(SPECTRUM.graphCountOffset)) !==
          SPECTRUM.graphCount ||
        ![SPECTRUM.centerAlignment, SPECTRUM.fitAlignment].includes(
          horizontalAlignment) ||
        data.readInt32LE(SPECTRUM.verticalAlignmentOffset) !==
          SPECTRUM.centerAlignment) {
      throw new Error("Spectrum graph alignment schema drifted.");
    }
    const peakMode = data.readInt32LE(219);
    const holdMs = data.readDoubleLE(223);
    const acceleration = data.readDoubleLE(231);
    if (![1, 2].includes(peakMode) ||
        ![0, 30].some((value) => nearlyEqual(holdMs, value)) ||
        !nearlyEqual(acceleration, 0.5)) {
      throw new Error(
        `Unexpected peak settings: mode=${peakMode}, hold=${holdMs}, acceleration=${acceleration}.`);
    }
    data.writeInt32LE(2, 219); // Gravity
    data.writeDoubleLE(0, 223); // No hold, matching foo_musical_spectrum.
    data.writeDoubleLE(0.5, 231); // 500 dB/s^2 in the Neko fork.
    data.writeUInt32LE(SPECTRUM.minimumNote, SPECTRUM.minNoteOffset);
    data.writeUInt32LE(SPECTRUM.maximumNote, SPECTRUM.maxNoteOffset);
    data.writeInt32LE(
      SPECTRUM.fitAlignment, SPECTRUM.horizontalAlignmentOffset);
    tuned += 1;
  }
  for (const child of panel.children?.children ?? []) tunePanel(child);
}

for (const preset of layout.presets) tunePanel(preset);
if (tuned === 0) throw new Error("Spectrum Analyzer panel was not found.");

dataset.data = buildLayout(layout, splitterGuids);
const result = buildFcl(fcl);
fs.writeFileSync(output, result);
console.log(JSON.stringify({
  schema: "nekorox.spectrum-legacy-response.v1",
  input,
  output,
  panels: tuned,
  sha256: crypto.createHash("sha256").update(result).digest("hex"),
}, null, 2));
