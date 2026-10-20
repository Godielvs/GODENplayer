"use strict";

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");
const { configureLocalOnly } = require("./openlyrics-config");

const [, , configArgument, manifestArgument] = process.argv;
if (!configArgument) {
  throw new Error(
    "Usage: node configure-openlyrics-local-only.js <foo_openlyrics.dll.cfg> [manifest.json]");
}

const configPath = path.resolve(configArgument);
const manifestPath = manifestArgument
  ? path.resolve(manifestArgument)
  : `${configPath}.local-only.json`;
const beforeBuffer = fs.readFileSync(configPath);
const result = configureLocalOnly(beforeBuffer);
const sha256 = (buffer) =>
  crypto.createHash("sha256").update(buffer).digest("hex").toUpperCase();

if (result.changed) {
  const temporary = `${configPath}.nekorox-next`;
  fs.writeFileSync(temporary, result.output);
  fs.renameSync(temporary, configPath);
}

const manifest = {
  schema: "nekorox.openlyrics-local-only.v1",
  configPath,
  changed: result.changed,
  beforeSources: result.before,
  afterSources: result.after,
  beforeSha256: sha256(beforeBuffer),
  afterSha256: sha256(result.output),
};
fs.mkdirSync(path.dirname(manifestPath), { recursive: true });
fs.writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`);
console.log(JSON.stringify(manifest, null, 2));
