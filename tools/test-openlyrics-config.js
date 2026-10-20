"use strict";

const assert = require("assert");
const {
  ACTIVE_SOURCES_GUID,
  SOURCE_GENERATION_GUID,
  LOCAL_FILES_GUID,
  METADATA_TAGS_GUID,
  buildComponentConfig,
  configureLocalOnly,
  decodeGuidList,
  encodeGuidList,
  parseComponentConfig,
} = require("./openlyrics-config");

const remoteOne = "4b0b5722-3a84-4b8e-827a-26b9eab3b4e8";
const remoteTwo = "aac13215-e32e-4667-acd7-1f0dbd8427e4";
const unknownGuid = "11111111-2222-3333-4455-66778899aabb";
const generation = Buffer.alloc(8);
generation.writeBigUInt64LE(7n, 0);
const fixture = buildComponentConfig({
  prefix: Buffer.alloc(32, 0x5a),
  records: [
    { guid: unknownGuid, data: Buffer.from("keep-me") },
    {
      guid: ACTIVE_SOURCES_GUID,
      data: encodeGuidList([
        LOCAL_FILES_GUID,
        METADATA_TAGS_GUID,
        remoteOne,
        remoteTwo,
      ]),
    },
    { guid: SOURCE_GENERATION_GUID, data: generation },
  ],
});

const first = configureLocalOnly(fixture);
assert.strictEqual(first.changed, true);
const parsed = parseComponentConfig(first.output);
assert.strictEqual(
  parsed.records.find((record) => record.guid === unknownGuid).data.toString(),
  "keep-me");
assert.deepStrictEqual(
  decodeGuidList(parsed.records.find(
    (record) => record.guid === ACTIVE_SOURCES_GUID).data),
  [LOCAL_FILES_GUID, METADATA_TAGS_GUID]);
assert.strictEqual(
  parsed.records.find(
    (record) => record.guid === SOURCE_GENERATION_GUID).data.readBigUInt64LE(0),
  8n);

const second = configureLocalOnly(first.output);
assert.strictEqual(second.changed, false);
assert.deepStrictEqual(second.output, first.output);
console.log("OpenLyrics local-only config transform: PASS");
