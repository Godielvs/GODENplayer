"use strict";

const crypto = require("crypto");
const fs = require("fs");
const path = require("path");
const { parseFcl } = require("./fcl-codec");

const LAYOUT_DATASET = "2cf00365-f2d7-4e78-9fca-c1d56e2707d8";
const [, , expectedArgument, actualArgument] = process.argv;
if (!expectedArgument || !actualArgument) {
  throw new Error("Usage: node verify-runtime-layout.js <expected.fcl> <actual.fcl>");
}

function layoutData(file) {
  const fcl = parseFcl(fs.readFileSync(file));
  const dataset = fcl.datasets.find((entry) => entry.guid === LAYOUT_DATASET);
  if (!dataset) throw new Error(`Columns UI layout dataset is missing from ${file}.`);
  return dataset.data;
}

function sha256(buffer) {
  return crypto.createHash("sha256").update(buffer).digest("hex");
}

const expectedPath = path.resolve(expectedArgument);
const actualPath = path.resolve(actualArgument);
const expected = layoutData(expectedPath);
const actual = layoutData(actualPath);
if (!actual.equals(expected)) {
  throw new Error(
    `Columns UI runtime layout mismatch: expected ${sha256(expected)}, actual ${sha256(actual)}.`);
}

console.log(JSON.stringify({
  schema: "nekorox.columns-runtime-layout.v1",
  expected: expectedPath,
  actual: actualPath,
  bytes: actual.length,
  sha256: sha256(actual),
}, null, 2));
