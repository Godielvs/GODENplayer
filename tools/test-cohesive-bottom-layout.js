"use strict";

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");
const { parseFcl, parseLayout } = require("./fcl-codec");

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

const [, , inputArgument] = process.argv;
if (!inputArgument) {
  throw new Error("Usage: node test-cohesive-bottom-layout.js <candidate.fcl>");
}

const candidate = path.resolve(inputArgument);
const fcl = parseFcl(fs.readFileSync(candidate));
const dataset = fcl.datasets.find(
  (entry) => entry.guid === GUID.layoutDataset);
if (!dataset) throw new Error("Columns UI layout dataset is missing.");
if (!fcl.panels.some((panel) => panel.guid === GUID.caption)) {
  throw new Error("NekoRoX Track Caption is missing from the dependency table.");
}

const layout = parseLayout(
  dataset.data,
  new Set([GUID.column, GUID.row]));
const preset = layout.presets[layout.active] ?? layout.presets[0];
const root = preset?.children?.children;
if (!root) throw new Error("Active layout has no root children.");

function guidBytes(guid) {
  const match = guid.match(
    /^([0-9a-f]{8})-([0-9a-f]{4})-([0-9a-f]{4})-([0-9a-f]{4})-([0-9a-f]{12})$/,
  );
  if (!match) throw new Error(`Invalid GUID: ${guid}`);
  const bytes = Buffer.alloc(16);
  bytes.writeUInt32LE(parseInt(match[1], 16), 0);
  bytes.writeUInt16LE(parseInt(match[2], 16), 4);
  bytes.writeUInt16LE(parseInt(match[3], 16), 6);
  Buffer.from(`${match[4]}${match[5]}`, "hex").copy(bytes, 8);
  return bytes;
}

function shellChildData(shell, guid) {
  const version = shell.childData.readUInt32LE(0);
  if (version < 2) return null;
  let cursor = version === 3 ? 16 : 12;
  const count = shell.childData.readUInt32LE(version === 3 ? 12 : 8);
  const wanted = guidBytes(guid);
  for (let index = 0; index < count; ++index) {
    if (cursor + 20 > shell.childData.length) {
      throw new Error("GODENplayer Shell child table is truncated.");
    }
    const entryGuid = shell.childData.subarray(cursor, cursor + 16);
    const length = shell.childData.readUInt32LE(cursor + 16);
    cursor += 20;
    if (cursor + length > shell.childData.length) {
      throw new Error("GODENplayer Shell child payload is truncated.");
    }
    const data = shell.childData.subarray(cursor, cursor + length);
    if (entryGuid.equals(wanted)) return data;
    cursor += length;
  }
  return null;
}

const shell = root.find((panel) => panel.guid === GUID.shell);
if (shell && shell.childData.length >= 4 &&
    [2, 3].includes(shell.childData.readUInt32LE(0))) {
  if (root.length !== 2 || root[1] !== shell) {
    throw new Error(
      "Legacy bottom panels remain outside GODENplayer Shell and would render a duplicate deck.");
  }
  for (const guid of [GUID.transport, GUID.waveSeek, GUID.spectrum,
    GUID.caption, GUID.cover]) {
    if (!shellChildData(shell, guid)) {
      throw new Error(`GODENplayer Shell is missing cohesive child ${guid}.`);
    }
  }
  const spectrumHash = crypto
    .createHash("sha256")
    .update(shellChildData(shell, GUID.spectrum))
    .digest("hex");
  const expectedSpectrumHashes = new Set([
    // Authored development payload (contains a local length-prefixed cache path).
    "884e348115386bb786fb30b3f60fca772cc770714d1882c1d6385def3db0c89e",
    // Release sanitizer removes that path while preserving every visual setting.
    "3a754c68d505562e2a6d3fee53486ee67be7b0898a0d37ebb35130735eb8c40c",
  ]);
  if (!expectedSpectrumHashes.has(spectrumHash)) {
    throw new Error(`Spectrum payload drifted: ${spectrumHash}`);
  }
  console.log("NekoRoX cohesive bottom layout: PASS");
  process.exit(0);
}
if (root.some((panel) => panel.guid === GUID.waveSeek)) {
  throw new Error("WaveSeek still spans the full root width.");
}

const transportPosition = root.findIndex(
  (panel) => panel.guid === GUID.transport);
const bottom = root[transportPosition + 1];
if (transportPosition < 0 || !bottom || bottom.guid !== GUID.row) {
  throw new Error("The cohesive bottom row is not directly below transport.");
}
const [leftColumn, cover] = bottom.children?.children ?? [];
if (!leftColumn || leftColumn.guid !== GUID.column ||
    !cover || cover.guid !== GUID.cover) {
  throw new Error("Bottom row is not the expected left-grid/cover composition.");
}
const left = leftColumn.children?.children ?? [];
const expected = [GUID.waveSeek, GUID.spectrum, GUID.caption];
if (!expected.every((guid, index) => left[index]?.guid === guid)) {
  throw new Error("Left bottom grid is not WaveSeek/Spectrum/Caption in order.");
}
if (left[0].size !== 36 || left[2].size !== 22) {
  throw new Error(
    `Bottom metrics drifted: WaveSeek=${left[0].size}, Caption=${left[2].size}.`);
}
if (left.reduce((sum, panel) => sum + panel.size, 0) !== bottom.size) {
  throw new Error("Left bottom grid does not exactly fill the bottom row.");
}
if (cover.size !== 220) {
  throw new Error(`Cover width drifted: ${cover.size}`);
}
if (!cover.locked) {
  throw new Error("Cover width must remain DPI-aware and fixed while the outer window resizes.");
}

const spectrumHash = crypto
  .createHash("sha256")
  .update(left[1].childData)
  .digest("hex");
const expectedSpectrumHashes = new Set([
  "884e348115386bb786fb30b3f60fca772cc770714d1882c1d6385def3db0c89e",
  "3a754c68d505562e2a6d3fee53486ee67be7b0898a0d37ebb35130735eb8c40c",
]);
if (!expectedSpectrumHashes.has(spectrumHash)) {
  throw new Error(`Spectrum payload drifted: ${spectrumHash}`);
}

console.log("NekoRoX cohesive bottom layout: PASS");
