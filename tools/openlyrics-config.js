"use strict";

const ACTIVE_SOURCES_GUID = "7d3c9b2c-b87b-4250-9956-8df580c92f39";
const SOURCE_GENERATION_GUID = "9046aa4a-352e-4467-bcd2-c41947d2bf24";
const LOCAL_FILES_GUID = "76d90970-1c98-4fe2-944e-ace493f38e85";
const METADATA_TAGS_GUID = "3fb0f715-a097-493a-944e-db4866088678";

function guidToBuffer(guid) {
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

function bufferToGuid(buffer, offset = 0) {
  if (offset < 0 || offset + 16 > buffer.length) {
    throw new Error("Truncated GUID.");
  }
  const d1 = buffer.readUInt32LE(offset).toString(16).padStart(8, "0");
  const d2 = buffer.readUInt16LE(offset + 4).toString(16).padStart(4, "0");
  const d3 = buffer.readUInt16LE(offset + 6).toString(16).padStart(4, "0");
  const tail = buffer.subarray(offset + 8, offset + 16).toString("hex");
  return `${d1}-${d2}-${d3}-${tail.slice(0, 4)}-${tail.slice(4)}`;
}

function u32(value) {
  const output = Buffer.alloc(4);
  output.writeUInt32LE(value >>> 0, 0);
  return output;
}

function parseComponentConfig(buffer) {
  if (!Buffer.isBuffer(buffer) || buffer.length < 36) {
    throw new Error("OpenLyrics component config is truncated.");
  }
  const payloadLength = buffer.readUInt32LE(32);
  if (payloadLength !== buffer.length - 36) {
    throw new Error(
      `OpenLyrics component config length mismatch: ${payloadLength} != ${buffer.length - 36}.`);
  }

  const records = [];
  let offset = 36;
  while (offset < buffer.length) {
    if (offset + 20 > buffer.length) {
      throw new Error(`Truncated config record at byte ${offset}.`);
    }
    const guid = bufferToGuid(buffer, offset);
    const length = buffer.readUInt32LE(offset + 16);
    const dataOffset = offset + 20;
    const end = dataOffset + length;
    if (end > buffer.length) {
      throw new Error(`Config record ${guid} exceeds the file boundary.`);
    }
    records.push({ guid, data: Buffer.from(buffer.subarray(dataOffset, end)) });
    offset = end;
  }
  return {
    prefix: Buffer.from(buffer.subarray(0, 32)),
    records,
  };
}

function buildComponentConfig(config) {
  const encodedRecords = config.records.map((record) => Buffer.concat([
    guidToBuffer(record.guid),
    u32(record.data.length),
    record.data,
  ]));
  const payload = Buffer.concat(encodedRecords);
  return Buffer.concat([config.prefix, u32(payload.length), payload]);
}

function decodeGuidList(data) {
  if (data.length < 4) throw new Error("OpenLyrics source list is truncated.");
  const count = data.readUInt32LE(0);
  if (data.length !== 4 + count * 16) {
    throw new Error("OpenLyrics source list length is inconsistent.");
  }
  const values = [];
  for (let index = 0; index < count; ++index) {
    values.push(bufferToGuid(data, 4 + index * 16));
  }
  return values;
}

function encodeGuidList(values) {
  return Buffer.concat([
    u32(values.length),
    ...values.map(guidToBuffer),
  ]);
}

function configureLocalOnly(buffer) {
  const config = parseComponentConfig(buffer);
  const sources = config.records.find(
    (record) => record.guid === ACTIVE_SOURCES_GUID);
  const generation = config.records.find(
    (record) => record.guid === SOURCE_GENERATION_GUID);
  if (!sources) {
    throw new Error("OpenLyrics active-source record was not found.");
  }
  if (!generation || generation.data.length !== 8) {
    throw new Error("OpenLyrics source-generation record is missing or incompatible.");
  }

  const before = decodeGuidList(sources.data);
  const after = [LOCAL_FILES_GUID, METADATA_TAGS_GUID];
  const changed =
    before.length !== after.length ||
    before.some((value, index) => value !== after[index]);
  if (changed) {
    sources.data = encodeGuidList(after);
    const nextGeneration = generation.data.readBigUInt64LE(0) + 1n;
    generation.data = Buffer.alloc(8);
    generation.data.writeBigUInt64LE(nextGeneration, 0);
  }

  const output = buildComponentConfig(config);
  const verified = parseComponentConfig(output);
  const verifiedSources = verified.records.find(
    (record) => record.guid === ACTIVE_SOURCES_GUID);
  if (!verifiedSources ||
      JSON.stringify(decodeGuidList(verifiedSources.data)) !==
        JSON.stringify(after)) {
    throw new Error("OpenLyrics local-only verification failed.");
  }
  return { output, changed, before, after };
}

module.exports = {
  ACTIVE_SOURCES_GUID,
  SOURCE_GENERATION_GUID,
  LOCAL_FILES_GUID,
  METADATA_TAGS_GUID,
  guidToBuffer,
  parseComponentConfig,
  buildComponentConfig,
  decodeGuidList,
  encodeGuidList,
  configureLocalOnly,
};
