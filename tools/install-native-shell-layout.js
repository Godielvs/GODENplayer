"use strict";

const fs = require("fs");
const path = require("path");
const { buildFcl, buildLayout, makePanel, parseFcl, parseLayout,
  pruneUnreferencedPanels } = require("./fcl-codec");

const GUID = {
  column: "77653a44-66d1-49e0-9a7a-1c71898c0441",
  row: "8fa0bc24-882a-4fff-8a3b-215ea7fbd07f",
  workspace: "6c52b18d-4664-4e8f-8dcd-ce2b82b76b32",
  shell: "a3dc5b91-875e-4f0a-a743-7d3183e6f42c",
  header: "c5c3917c-8030-46b5-afc8-9ec5dca17c19",
  transport: "37dfaaa1-5040-4966-819f-d012f41a6078",
  waveSeek: "369b09d4-58a7-4730-a2ce-ddd824e83213",
  legacyWaveform: "8b15938a-a993-4101-8441-f84fe0532846",
  spectrum: "3247c894-e585-4025-a866-c7d4933fb2e3",
  caption: "6d9a108b-cc44-44ba-8854-5f7bd0cd3310",
  cover: "5d0cc69e-d3d0-4c82-90db-40c7cf9c3c9e",
  layoutDataset: "2cf00365-f2d7-4e78-9fca-c1d56e2707d8",
};

const [, , inputArgument, outputArgument] = process.argv;
if (!inputArgument || !outputArgument) {
  throw new Error("Usage: node install-native-shell-layout.js <input.fcl> <output.fcl>");
}

function u32(value) {
  const output = Buffer.alloc(4);
  output.writeUInt32LE(value >>> 0, 0);
  return output;
}

function i32(value) {
  const output = Buffer.alloc(4);
  output.writeInt32LE(value | 0, 0);
  return output;
}

function guidBytes(guid) {
  const match = guid.toLowerCase().match(
    /^([0-9a-f]{8})-([0-9a-f]{4})-([0-9a-f]{4})-([0-9a-f]{4})-([0-9a-f]{12})$/);
  if (!match) throw new Error(`Invalid GUID: ${guid}`);
  const output = Buffer.alloc(16);
  output.writeUInt32LE(parseInt(match[1], 16), 0);
  output.writeUInt16LE(parseInt(match[2], 16), 4);
  output.writeUInt16LE(parseInt(match[3], 16), 6);
  Buffer.from(`${match[4]}${match[5]}`, "hex").copy(output, 8);
  return output;
}

function payloadV3(entries, mode = 0, spectrumHeightDip = 152) {
  return Buffer.concat([u32(3), u32(mode), i32(spectrumHeightDip),
    u32(entries.length),
    ...entries.flatMap(({ guid, data }) => [guidBytes(guid), u32(data.length), data])]);
}

function panelLike(source, guid, size) {
  const result = makePanel(guid, size, source.extraDpi ?? 96);
  for (const key of ["captionOrientation", "locked", "hidden", "showCaption",
    "autohide", "showToggleArea"]) result[key] = source[key];
  return result;
}

const input = path.resolve(inputArgument);
const output = path.resolve(outputArgument);
const splitters = new Set([GUID.column, GUID.row]);
const fcl = parseFcl(fs.readFileSync(input));
const dataset = fcl.datasets.find((entry) => entry.guid === GUID.layoutDataset);
if (!dataset) throw new Error("Columns UI layout dataset was not found.");

function register(guid, name) {
  let index = fcl.panels.findIndex((panel) => panel.guid === guid);
  if (index < 0) {
    index = fcl.panels.length;
    fcl.panels.push({ guid, name });
  }
  if (!dataset.required.includes(index)) dataset.required.push(index);
}
register(GUID.shell, "GODENplayer Shell V2");
register(GUID.workspace, "GODENplayer Workspace");
register(GUID.transport, "NekoRoX Transport");
register(GUID.waveSeek, "NekoRoX WaveSeek");
register(GUID.spectrum, "Musical Spectrum");
register(GUID.caption, "NekoRoX Track Caption");
register(GUID.cover, "NekoRoX Cover");

const layout = parseLayout(dataset.data, splitters);
const preset = layout.presets[layout.active] ?? layout.presets[0];
const root = preset?.children?.children;
if (!root || preset.guid !== GUID.column) {
  throw new Error("Active preset is not the expected vertical root splitter.");
}

let shellIndex = root.findIndex((panel) => panel.guid === GUID.shell);
let sourceKind = "old";
if (shellIndex >= 0 && root[shellIndex].childData.length >= 4 &&
    root[shellIndex].childData.readUInt32LE(0) === 3) {
  sourceKind = "v3";
} else if (shellIndex >= 0 && root[shellIndex].childData.length >= 12 &&
    root[shellIndex].childData.readUInt32LE(0) === 2) {
  sourceKind = "v2";
  const oldPayload = root[shellIndex].childData;
  root[shellIndex].childData = Buffer.concat([
    u32(3),
    Buffer.from(oldPayload.subarray(4, 8)),
    i32(152),
    Buffer.from(oldPayload.subarray(8)),
  ]);
} else {
  let workspaceIndex = root.findIndex((panel) => panel.guid === GUID.workspace);
  let workspaceData;
  if (shellIndex >= 0) {
    const oldShell = root[shellIndex];
    if (oldShell.childData.length < 8 || oldShell.childData.readUInt32LE(0) !== 1) {
      throw new Error("Existing shell payload is neither V1 nor V2.");
    }
    sourceKind = "v1";
    workspaceIndex = shellIndex;
    workspaceData = Buffer.from(oldShell.childData.subarray(8));
  } else if (workspaceIndex >= 0) {
    workspaceData = Buffer.from(root[workspaceIndex].childData);
  } else throw new Error("Workspace or PlayerShell V1 was not found.");

  const transport = root[workspaceIndex + 1];
  const bottom = root[workspaceIndex + 2];
  if (!transport || transport.guid !== GUID.transport || !bottom) {
    throw new Error("Transport/bottom subtree is not adjacent to the workspace.");
  }
  let wave, spectrum, caption, cover;
  let consumed = 3;
  if (bottom.guid === GUID.row && bottom.children) {
    const left = bottom.children.children.find((panel) => panel.guid === GUID.column);
    cover = bottom.children.children.find((panel) => panel.guid === GUID.cover);
    wave = left?.children?.children.find((panel) => panel.guid === GUID.waveSeek);
    spectrum = left?.children?.children.find((panel) => panel.guid === GUID.spectrum);
    caption = left?.children?.children.find((panel) => panel.guid === GUID.caption);
  } else if (bottom.guid === GUID.waveSeek ||
      bottom.guid === GUID.legacyWaveform) {
    wave = bottom.guid === GUID.waveSeek
      ? bottom : makePanel(GUID.waveSeek, 36, 96);
    const legacyRow = root[workspaceIndex + 3];
    if (legacyRow?.guid !== GUID.row || !legacyRow.children) {
      throw new Error("Legacy WaveSeek is not followed by spectrum/cover row.");
    }
    spectrum = legacyRow.children.children.find((panel) => panel.guid === GUID.spectrum);
    cover = legacyRow.children.children.find((panel) => panel.guid === GUID.cover);
    caption = makePanel(GUID.caption, 22, 96);
    consumed = 4;
  }
  if (!wave || !spectrum || !caption || !cover) {
    throw new Error("Complete WaveSeek/Spectrum/Caption/Cover subtree was not found.");
  }
  const source = root[workspaceIndex];
  const totalSize = root.slice(workspaceIndex, workspaceIndex + consumed)
    .reduce((sum, panel) => sum + panel.size, 0);
  const shell = panelLike(source, GUID.shell, totalSize);
  shell.childData = payloadV3([
    { guid: GUID.workspace, data: workspaceData },
    { guid: GUID.transport, data: Buffer.from(transport.childData) },
    { guid: GUID.waveSeek, data: Buffer.from(wave.childData) },
    { guid: GUID.spectrum, data: Buffer.from(spectrum.childData) },
    { guid: GUID.caption, data: Buffer.from(caption.childData) },
    { guid: GUID.cover, data: Buffer.from(cover.childData) },
  ]);
  root.splice(workspaceIndex, consumed, shell);
  shellIndex = workspaceIndex;
}

const headerIndex = root.findIndex((panel) => panel.guid === GUID.header);
if (headerIndex < 0 || shellIndex !== headerIndex + 1) {
  throw new Error("Header must be directly above PlayerShell V2.");
}
root[headerIndex].size = 70;
root[headerIndex].extraSize = 70;
dataset.data = buildLayout(layout, splitters);
const removedPanels = pruneUnreferencedPanels(fcl);
fs.mkdirSync(path.dirname(output), { recursive: true });
const backup = `${output}.before-player-shell-v2.fcl`;
if (!fs.existsSync(backup)) fs.copyFileSync(input, backup, fs.constants.COPYFILE_EXCL);
fs.writeFileSync(output, buildFcl(fcl));
console.log(JSON.stringify({ schema: "goden.player-shell-layout.v3", sourceKind,
  input, output, backup, shellIndex,
  rootChildren: root.map((panel) => ({ guid: panel.guid, size: panel.size })),
  removedPanels }, null, 2));
