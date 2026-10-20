"use strict";

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");
const { parseFcl, parseLayout } = require("./fcl-codec");

const LAYOUT_DATASET = "2cf00365-f2d7-4e78-9fca-c1d56e2707d8";
const COLUMN = "77653a44-66d1-49e0-9a7a-1c71898c0441";
const ROW = "8fa0bc24-882a-4fff-8a3b-215ea7fbd07f";
const WORKSPACE = "6c52b18d-4664-4e8f-8dcd-ce2b82b76b32";
const PLAYER_SHELL = "a3dc5b91-875e-4f0a-a743-7d3183e6f42c";
const [, , configArgument, layoutArgument, reportArgument, approvedHashArgument] =
  process.argv;
if (!configArgument || !layoutArgument || !reportArgument) {
  throw new Error(
    "Usage: node verify-columns-config-seed.js <foo_ui_columns.dll.cfg> " +
      "<layout.fcl> <report.json> [runtime-approved-sha256]",
  );
}

function sha256(buffer) {
  return crypto.createHash("sha256").update(buffer).digest("hex").toUpperCase();
}

const configPath = path.resolve(configArgument);
const layoutPath = path.resolve(layoutArgument);
const reportPath = path.resolve(reportArgument);
const config = fs.readFileSync(configPath);
const configHash = sha256(config);
if (
  approvedHashArgument &&
  configHash !== approvedHashArgument.trim().toUpperCase()
) {
  throw new Error(
    `Columns UI seed was not accepted by the runtime gate: ${configHash}.`,
  );
}
const fcl = parseFcl(fs.readFileSync(layoutPath));
const dataset = fcl.datasets.find((entry) => entry.guid === LAYOUT_DATASET);
if (!dataset) throw new Error("Columns UI layout dataset is missing from the FCL.");
if (config.length < 36 || config.readUInt32LE(32) !== config.length - 36) {
  throw new Error("Columns UI seed has invalid outer framing.");
}

const splitterGuids = new Set([COLUMN, ROW]);
const candidates = [];
let cursor = 36;
while (cursor < config.length) {
  if (cursor + 20 > config.length) throw new Error(`Truncated record at ${cursor}.`);
  const length = config.readUInt32LE(cursor + 16);
  const dataOffset = cursor + 20;
  const end = dataOffset + length;
  if (end > config.length) throw new Error(`Record at ${cursor} exceeds the seed.`);
  const data = config.subarray(dataOffset, end);
  try {
    const parsed = parseLayout(data, splitterGuids);
    const preset = parsed.presets[parsed.active] ?? parsed.presets[0];
    if (
      preset?.guid === COLUMN &&
      preset.children?.children.some(
        (panel) => panel.guid === WORKSPACE || panel.guid === PLAYER_SHELL,
      )
    ) {
      candidates.push({ offset: cursor, data });
    }
  } catch {
  }
  cursor = end;
}
if (cursor !== config.length) throw new Error("Columns UI seed framing did not terminate cleanly.");
if (candidates.length !== 1) {
  throw new Error(`Expected one NekoRoX layout record; found ${candidates.length}.`);
}
if (!candidates[0].data.equals(dataset.data)) {
  throw new Error("Columns UI seed layout does not match the sanitized FCL.");
}

const report = {
  schema: "nekorox.columns-config-seed.v1",
  config: configPath,
  layout: layoutPath,
  configBytes: config.length,
  configSha256: configHash,
  layoutBytes: dataset.data.length,
  layoutSha256: sha256(dataset.data),
  recordOffset: candidates[0].offset,
  nativeRuntimeAccepted: Boolean(approvedHashArgument),
  runtimeApprovedSha256: approvedHashArgument
    ? approvedHashArgument.trim().toUpperCase()
    : null,
};
fs.mkdirSync(path.dirname(reportPath), { recursive: true });
fs.writeFileSync(reportPath, `${JSON.stringify(report, null, 2)}\n`);
console.log(JSON.stringify(report, null, 2));
