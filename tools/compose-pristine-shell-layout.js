"use strict";

const fs = require("fs");
const path = require("path");
const { buildFcl, buildLayout, parseFcl, parseLayout } = require("./fcl-codec");

const GUID = {
  column: "77653a44-66d1-49e0-9a7a-1c71898c0441",
  row: "8fa0bc24-882a-4fff-8a3b-215ea7fbd07f",
  shell: "a3dc5b91-875e-4f0a-a743-7d3183e6f42c",
  workspace: "6c52b18d-4664-4e8f-8dcd-ce2b82b76b32",
  layoutDataset: "2cf00365-f2d7-4e78-9fca-c1d56e2707d8",
};

const [, , desiredArgument, pristineArgument, outputArgument] = process.argv;
if (!desiredArgument || !pristineArgument || !outputArgument) {
  throw new Error(
    "Usage: node compose-pristine-shell-layout.js " +
      "<desired-shell.fcl> <pristine-shell.fcl> <output.fcl>",
  );
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

function u32(value) {
  const bytes = Buffer.alloc(4);
  bytes.writeUInt32LE(value >>> 0);
  return bytes;
}

function shellFrom(file) {
  const fcl = parseFcl(fs.readFileSync(file));
  const dataset = fcl.datasets.find((entry) => entry.guid === GUID.layoutDataset);
  if (!dataset) throw new Error(`Layout dataset is missing from ${file}.`);
  const splitters = new Set([GUID.column, GUID.row]);
  const layout = parseLayout(dataset.data, splitters);
  const preset = layout.presets[layout.active] ?? layout.presets[0];
  const shell = preset?.children?.children.find((panel) => panel.guid === GUID.shell);
  if (!shell || shell.childData.length < 16 || shell.childData.readUInt32LE(0) !== 3) {
    throw new Error(`PlayerShell V3 is missing from ${file}.`);
  }
  const count = shell.childData.readUInt32LE(12);
  const entries = [];
  let cursor = 16;
  for (let index = 0; index < count; ++index) {
    if (cursor + 20 > shell.childData.length) throw new Error("Shell table is truncated.");
    const guid = Buffer.from(shell.childData.subarray(cursor, cursor + 16));
    const length = shell.childData.readUInt32LE(cursor + 16);
    cursor += 20;
    if (cursor + length > shell.childData.length) throw new Error("Shell payload is truncated.");
    entries.push({ guid, data: Buffer.from(shell.childData.subarray(cursor, cursor + length)) });
    cursor += length;
  }
  if (cursor !== shell.childData.length) throw new Error("Shell table has trailing bytes.");
  return { fcl, dataset, splitters, layout, shell, entries };
}

const desiredPath = path.resolve(desiredArgument);
const pristinePath = path.resolve(pristineArgument);
const outputPath = path.resolve(outputArgument);
const desired = shellFrom(desiredPath);
const pristine = shellFrom(pristinePath);
const workspace = guidBytes(GUID.workspace);
const cleanWorkspace = pristine.entries.find((entry) => entry.guid.equals(workspace));
if (!cleanWorkspace) throw new Error("Pristine shell has no Workspace child.");
const entries = desired.entries.map((entry) =>
  entry.guid.equals(workspace) ? { guid: entry.guid, data: cleanWorkspace.data } : entry,
);
if (!entries.some((entry) => entry.guid.equals(workspace))) {
  throw new Error("Desired shell has no Workspace child.");
}
const header = Buffer.alloc(16);
header.writeUInt32LE(3, 0);
header.writeUInt32LE(0, 4);
header.writeInt32LE(152, 8);
header.writeUInt32LE(entries.length, 12);
desired.shell.childData = Buffer.concat([
  header,
  ...entries.flatMap((entry) => [entry.guid, u32(entry.data.length), entry.data]),
]);
desired.dataset.data = buildLayout(desired.layout, desired.splitters);
fs.mkdirSync(path.dirname(outputPath), { recursive: true });
fs.writeFileSync(outputPath, buildFcl(desired.fcl));
console.log(JSON.stringify({
  schema: "goden.pristine-shell-layout.v1",
  desired: desiredPath,
  pristine: pristinePath,
  output: outputPath,
  workspaceBytes: cleanWorkspace.data.length,
  childCount: entries.length,
}, null, 2));
