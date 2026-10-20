"use strict";

const crypto = require("crypto");
const fs = require("fs");

const [, , dllPath, manifestPath] = process.argv;
if (!dllPath || !manifestPath) {
  throw new Error("Usage: verify-openlyrics-local-only.js <dll> <manifest>");
}

function guidBytes(guid) {
  const parts = guid.match(
    /^([0-9a-f]{8})-([0-9a-f]{4})-([0-9a-f]{4})-([0-9a-f]{4})-([0-9a-f]{12})$/i);
  if (!parts) throw new Error(`Invalid GUID: ${guid}`);
  const bytes = Buffer.alloc(16);
  bytes.writeUInt32LE(Number.parseInt(parts[1], 16), 0);
  bytes.writeUInt16LE(Number.parseInt(parts[2], 16), 4);
  bytes.writeUInt16LE(Number.parseInt(parts[3], 16), 6);
  Buffer.from(parts[4] + parts[5], "hex").copy(bytes, 8);
  return bytes;
}

const local = "76d90970-1c98-4fe2-944e-ace493f38e85";
const metadata = "3fb0f715-a097-493a-944e-db4866088678";
const qqMusic = "4b0b5722-3a84-4b8e-827a-26b9eab3b4e8";
const netease = "aac13215-e32e-4667-acd7-1f0dbd8427e4";
const binary = fs.readFileSync(dllPath);
const localOnly = Buffer.concat([guidBytes(local), guidBytes(metadata)]);
const upstreamRemoteDefault = Buffer.concat([
  localOnly,
  guidBytes(qqMusic),
  guidBytes(netease),
]);

const localOnlyOffset = binary.indexOf(localOnly);
const remoteDefaultOffset = binary.indexOf(upstreamRemoteDefault);
if (localOnlyOffset < 0) {
  throw new Error("The local-files + metadata default GUID sequence was not found.");
}
if (remoteDefaultOffset >= 0) {
  throw new Error("The upstream remote-source default sequence is still present.");
}

const manifest = {
  schema: "nekorox.openlyrics-local-only.v1",
  upstream: "OpenLyrics 1.13",
  source: "https://github.com/jacquesh/foo_openlyrics/tree/v1.13",
  license: "MIT",
  defaultSources: ["local-files", "metadata-tags"],
  remoteSourcesRemainUserSelectable: true,
  localOnlySequenceOffset: localOnlyOffset,
  dllBytes: binary.length,
  dllSha256: crypto.createHash("sha256").update(binary).digest("hex").toUpperCase(),
};
fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2) + "\n");
