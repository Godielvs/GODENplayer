"use strict";

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");
const { parseFcl, parseLayout } = require("./fcl-codec");

const GUID = {
  column: "77653a44-66d1-49e0-9a7a-1c71898c0441",
  row: "8fa0bc24-882a-4fff-8a3b-215ea7fbd07f",
  header: "c5c3917c-8030-46b5-afc8-9ec5dca17c19",
  monitor: "8e75bd84-c4d0-4f59-8da8-9b1233f3ef71",
  workspace: "6c52b18d-4664-4e8f-8dcd-ce2b82b76b32",
  shell: "a3dc5b91-875e-4f0a-a743-7d3183e6f42c",
  spectrum: "3247c894-e585-4025-a866-c7d4933fb2e3",
  layoutDataset: "2cf00365-f2d7-4e78-9fca-c1d56e2707d8",
};

const repo = path.resolve(__dirname, "..");
const input = process.argv[2]
  ? path.resolve(process.argv[2])
  : path.join(repo, "config", "layout", "nekorox-v3.fcl");
const fcl = parseFcl(fs.readFileSync(input));
const dataset = fcl.datasets.find((entry) => entry.guid === GUID.layoutDataset);
if (!dataset) throw new Error("Columns UI layout dataset is missing.");

const monitorPanelIndex = fcl.panels.findIndex((entry) => entry.guid === GUID.monitor);
if (monitorPanelIndex >= 0) {
  throw new Error("The retired full-width monitor remains in the FCL dependency table.");
}
const referencedPanels = new Set(fcl.datasets.flatMap((entry) => entry.required));
const unreferencedPanels = fcl.panels.filter((_, index) => !referencedPanels.has(index));
if (unreferencedPanels.length !== 0) {
  throw new Error(`FCL contains unreferenced panel declarations: ${
    unreferencedPanels.map((panel) => panel.name).join(", ")}`);
}
if (fcl.panels.some((entry) => entry.name === "JScript Panel 3")) {
  throw new Error("Retired JScript Panel 3 remains in the FCL dependency table.");
}

const splitterGuids = new Set([GUID.column, GUID.row]);
const layout = parseLayout(dataset.data, splitterGuids);
const preset = layout.presets[layout.active] ?? layout.presets[0];
const root = preset?.children?.children;
if (!root) throw new Error("Active layout has no root children.");

const expectedTop = [GUID.header, GUID.shell];
if (!expectedTop.every((guid, index) => root[index]?.guid === guid)) {
  throw new Error("Integrated-monitor header and workspace are not adjacent and ordered.");
}
if (root.length !== expectedTop.length) {
  throw new Error(
    "Legacy transport/bottom panels remain beside GODENplayer Shell and would render a duplicate deck.");
}
if (root.some((panel) => panel.guid === GUID.workspace)) {
  throw new Error("Workspace is still a top-level panel instead of a retained shell child.");
}
if (root[1].childData.length < 8 ||
    ![1, 2, 3].includes(root[1].childData.readUInt32LE(0))) {
  throw new Error("GODENplayer Shell configuration envelope is missing.");
}
const shellVersion = root[1].childData.readUInt32LE(0);
if (shellVersion >= 2) {
  const countOffset = shellVersion === 3 ? 12 : 8;
  if (root[1].childData.length < countOffset + 4 ||
      root[1].childData.readUInt32LE(countOffset) !== 6) {
    throw new Error("GODENplayer Shell child configuration table is invalid.");
  }
}
if (root[0].size !== 70) {
  throw new Error(`FL-style header height drifted: ${root[0].size}`);
}

function findPanel(panels, guid) {
  for (const panel of panels) {
    if (panel.guid === guid) return panel;
    const nested = findPanel(panel.children?.children ?? [], guid);
    if (nested) return nested;
  }
  return null;
}

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

const nestedSpectrum = findPanel(root, GUID.spectrum)?.childData ?? null;
const spectrumData = nestedSpectrum ?? shellChildData(root[1], GUID.spectrum);
if (!spectrumData) throw new Error("Spectrum Analyzer is missing from the bottom band.");
const spectrumHash = crypto.createHash("sha256").update(spectrumData).digest("hex");
// The generic topology probe is also used by stage-dev before the specialized
// WaveSeek/cohesive-bottom contract. Those two presets intentionally share a
// newer Spectrum payload; keep each filename fail-closed to its own known
// payload instead of rejecting the valid candidate before its dedicated test.
const specializedBottom = /(?:waveseek|cohesive-bottom)/i.test(path.basename(input));
const expectedSpectrumHash = specializedBottom
  ? "884e348115386bb786fb30b3f60fca772cc770714d1882c1d6385def3db0c89e"
  : "3a754c68d505562e2a6d3fee53486ee67be7b0898a0d37ebb35130735eb8c40c";
if (spectrumHash !== expectedSpectrumHash) {
  throw new Error(`Spectrum payload drifted: ${spectrumHash}`);
}

console.log("NekoRoX layout tests: PASS");
