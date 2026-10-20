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

const LAYOUT_DATASET = "2cf00365-f2d7-4e78-9fca-c1d56e2707d8";
const PLAYER_SHELL = "a3dc5b91-875e-4f0a-a743-7d3183e6f42c";
const SPLITTER_GUIDS = new Set([
  "77653a44-66d1-49e0-9a7a-1c71898c0441",
  "8fa0bc24-882a-4fff-8a3b-215ea7fbd07f",
]);
const PRIVATE_STRINGS = [];

function addPrivateString(value) {
  if (!value) return;
  const normalized = path.resolve(value);
  if (!PRIVATE_STRINGS.some((item) => item.toLowerCase() === normalized.toLowerCase())) {
    PRIVATE_STRINGS.unshift(normalized);
  }
}

function sha256(buffer) {
  return crypto.createHash("sha256").update(buffer).digest("hex").toUpperCase();
}

function u32(value) {
  const output = Buffer.alloc(4);
  output.writeUInt32LE(value >>> 0);
  return output;
}

function containsPrivateData(buffer) {
  return PRIVATE_STRINGS.some((value) => buffer.includes(Buffer.from(value, "utf8")));
}

function stripKnownLengthPrefixedStrings(buffer) {
  let output = buffer;
  let removed = 0;
  for (const value of PRIVATE_STRINGS) {
    const encoded = Buffer.from(value, "utf8");
    for (;;) {
      const offset = output.indexOf(encoded);
      if (offset < 0) break;
      if (offset < 4 || output.readUInt32LE(offset - 4) !== encoded.length) {
        throw new Error(`Private string is not length-prefixed at byte ${offset}.`);
      }
      output = Buffer.concat([
        output.subarray(0, offset - 4),
        u32(0),
        output.subarray(offset + encoded.length),
      ]);
      removed += 1;
    }
  }
  return { output, removed };
}

function sanitizeShellChildData(buffer) {
  if (buffer.length < 8) {
    throw new Error("GODENplayer Shell payload is truncated.");
  }
  const version = buffer.readUInt32LE(0);
  if (version === 1) {
    const result = stripKnownLengthPrefixedStrings(buffer.subarray(8));
    return {
      output: Buffer.concat([buffer.subarray(0, 8), result.output]),
      removed: result.removed,
    };
  }
  if (version !== 2 && version !== 3) {
    throw new Error(`Unsupported GODENplayer Shell payload version: ${version}.`);
  }
  const countOffset = version === 3 ? 12 : 8;
  const entriesOffset = countOffset + 4;
  if (buffer.length < entriesOffset) {
    throw new Error("GODENplayer Shell payload header is truncated.");
  }
  const count = buffer.readUInt32LE(countOffset);
  const entries = [];
  let cursor = entriesOffset;
  let removed = 0;
  for (let index = 0; index < count; ++index) {
    if (cursor + 20 > buffer.length) {
      throw new Error("GODENplayer Shell child table is truncated.");
    }
    const guid = buffer.subarray(cursor, cursor + 16);
    const length = buffer.readUInt32LE(cursor + 16);
    cursor += 20;
    if (cursor + length > buffer.length) {
      throw new Error("GODENplayer Shell child payload is truncated.");
    }
    const result = stripKnownLengthPrefixedStrings(
      buffer.subarray(cursor, cursor + length));
    entries.push(Buffer.concat([guid, u32(result.output.length), result.output]));
    removed += result.removed;
    cursor += length;
  }
  if (cursor !== buffer.length) {
    throw new Error("GODENplayer Shell payload has trailing bytes.");
  }
  return {
    output: Buffer.concat([buffer.subarray(0, entriesOffset), ...entries]),
    removed,
  };
}

function sanitizePanels(panels) {
  let removed = 0;
  for (const panel of panels) {
    if (panel.children) {
      removed += sanitizePanels(panel.children.children);
      continue;
    }
    const result = panel.guid === PLAYER_SHELL
      ? sanitizeShellChildData(panel.childData)
      : stripKnownLengthPrefixedStrings(panel.childData);
    panel.childData = result.output;
    removed += result.removed;
  }
  return removed;
}

function sanitizeLayoutData(data) {
  const layout = parseLayout(data, SPLITTER_GUIDS);
  let removed = 0;
  for (const preset of layout.presets) {
    if (preset.children) removed += sanitizePanels(preset.children.children);
  }
  const output = buildLayout(layout, SPLITTER_GUIDS);
  if (containsPrivateData(output)) {
    throw new Error("Private path remains in transformed Columns UI layout data.");
  }
  return { output, removed };
}

function sanitizeFcl(file) {
  const before = fs.readFileSync(file);
  const fcl = parseFcl(before);
  const dataset = fcl.datasets.find((entry) => entry.guid === LAYOUT_DATASET);
  if (!dataset) throw new Error(`Columns UI layout dataset is missing from ${file}.`);
  const result = sanitizeLayoutData(dataset.data);
  dataset.data = result.output;
  const after = buildFcl(fcl);
  if (containsPrivateData(after)) throw new Error(`Private path remains in ${file}.`);
  fs.writeFileSync(file, after);
  return {
    file,
    removed: result.removed,
    beforeBytes: before.length,
    afterBytes: after.length,
    beforeSha256: sha256(before),
    afterSha256: sha256(after),
  };
}

function collectFiles(root) {
  const output = [];
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const full = path.join(root, entry.name);
    if (entry.isDirectory()) output.push(...collectFiles(full));
    else if (entry.isFile()) output.push(full);
  }
  return output;
}

function genericPrivateMatches(buffer) {
  const encodings = [buffer.toString("latin1"), buffer.toString("utf16le")];
  const patterns = [
    /[A-Za-z]:\\Users\\[^\\\x00-\x1f]{1,128}/gi,
    /\/Users\/[^/\x00-\x1f]{1,128}/gi,
    /\/home\/[^/\x00-\x1f]{1,128}/gi,
  ];
  const matches = new Set();
  for (const text of encodings) {
    for (const pattern of patterns) {
      pattern.lastIndex = 0;
      for (const match of text.matchAll(pattern)) {
        matches.add(match[0].slice(0, 180));
      }
    }
  }
  return [...matches];
}

module.exports = { addPrivateString, sanitizeFcl, genericPrivateMatches };
if (require.main === module) {
try {
const [, , profileArgument, manifestArgument, sourceAppArgument] = process.argv;
if (!profileArgument || !manifestArgument) {
  throw new Error("Usage: node sanitize-friend-profile.js <profile> <manifest.json>");
}

const profile = path.resolve(profileArgument);
const manifestPath = path.resolve(manifestArgument);
addPrivateString(process.env.USERPROFILE);
if (sourceAppArgument) {
  addPrivateString(sourceAppArgument);
  addPrivateString(path.join(sourceAppArgument, "profile"));
}
const layout = path.join(profile, "nekorox", "presets", "layout.fcl");
for (const required of [layout]) {
  if (!fs.existsSync(required)) throw new Error(`Required profile file is missing: ${required}`);
}

// Columns UI owns the serialization and integrity of foo_ui_columns.dll.cfg.
// The installer uses a seed emitted by Columns UI after importing this sanitized
// FCL, so this script intentionally never rewrites the proprietary config blob.
const transformations = [sanitizeFcl(layout)];
const leakDetails = [];
for (const file of collectFiles(profile)) {
  const bytes = fs.readFileSync(file);
  const known = containsPrivateData(bytes);
  const generic = genericPrivateMatches(bytes);
  if (known || generic.length > 0) {
    leakDetails.push({ file: path.relative(profile, file), known, genericMatches: generic.length });
  }
}
if (leakDetails.length > 0) {
  throw new Error(`Private paths remain in friend profile: ${JSON.stringify(leakDetails)}`);
}

const manifest = {
  schema: "nekorox.friend-profile-sanitization.v3",
  createdUtc: new Date().toISOString(),
  profile: "<staged-profile>",
  privateStringRulesChecked: PRIVATE_STRINGS.length,
  transformations: transformations.map((entry) => ({...entry, file: path.relative(profile, entry.file)})),
  scannedFiles: collectFiles(profile).length,
  genericHomePathPatternsChecked: true,
  leaks: [],
};
fs.mkdirSync(path.dirname(manifestPath), { recursive: true });
fs.writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
console.log(JSON.stringify(manifest, null, 2));
} catch {
  // Errors from fs/codecs can contain arbitrary input paths or marker values.
  // Public build logs must never echo them, including Node's default stack.
  console.error("Profile sanitization failed: invalid seed or private data remains. No private values are included in this diagnostic.");
  process.exitCode = 1;
}
}
