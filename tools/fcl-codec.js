"use strict";

function guidToString(buffer, offset) {
  const d1 = buffer.readUInt32LE(offset).toString(16).padStart(8, "0");
  const d2 = buffer.readUInt16LE(offset + 4).toString(16).padStart(4, "0");
  const d3 = buffer.readUInt16LE(offset + 6).toString(16).padStart(4, "0");
  const bytes = [...buffer.slice(offset + 8, offset + 16)]
    .map((value) => value.toString(16).padStart(2, "0"));
  return `${d1}-${d2}-${d3}-${bytes.slice(0, 2).join("")}-${bytes.slice(2).join("")}`;
}

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

class Reader {
  constructor(buffer, offset = 0, end = buffer.length) {
    this.buffer = buffer;
    this.offset = offset;
    this.end = end;
  }

  require(size) {
    if (this.offset + size > this.end) {
      throw new Error(`Truncated FCL payload at byte ${this.offset}.`);
    }
  }

  u32() {
    this.require(4);
    const value = this.buffer.readUInt32LE(this.offset);
    this.offset += 4;
    return value;
  }

  bool() {
    this.require(1);
    const value = this.buffer.readUInt8(this.offset) !== 0;
    this.offset += 1;
    return value;
  }

  guid() {
    this.require(16);
    const value = guidToString(this.buffer, this.offset);
    this.offset += 16;
    return value;
  }

  bytes(size) {
    this.require(size);
    const value = this.buffer.slice(this.offset, this.offset + size);
    this.offset += size;
    return value;
  }

  string() {
    return this.bytes(this.u32()).toString("utf8");
  }
}

function u32(value) {
  const output = Buffer.alloc(4);
  output.writeUInt32LE(value >>> 0, 0);
  return output;
}

function bool(value) {
  return Buffer.from([value ? 1 : 0]);
}

function string(value) {
  const encoded = Buffer.from(value, "utf8");
  return Buffer.concat([u32(encoded.length), encoded]);
}

function parsePanel(reader, splitterGuids) {
  const panel = {
    guid: reader.guid(),
    captionOrientation: reader.u32(),
    locked: reader.bool(),
    hidden: reader.bool(),
    showCaption: reader.bool(),
    autohide: reader.bool(),
    size: reader.u32(),
    showToggleArea: reader.bool(),
  };
  panel.childData = reader.bytes(reader.u32());
  panel.useCustomTitle = reader.bool();
  panel.customTitle = reader.string();
  panel.children = parseSplitter(panel.guid, panel.childData, splitterGuids);
  return panel;
}

function parseSplitter(guid, data, splitterGuids) {
  if (!splitterGuids.has(guid)) return null;
  const reader = new Reader(data);
  const version = reader.u32();
  const count = reader.u32();
  const children = [];
  for (let index = 0; index < count; ++index) {
    children.push(parsePanel(reader, splitterGuids));
  }
  for (let index = 0; index < count; ++index) {
    const extra = reader.bytes(reader.u32());
    if (extra.length >= 8) {
      children[index].extraSize = extra.readUInt32LE(0);
      children[index].extraDpi = extra.readUInt32LE(4);
    } else {
      children[index].extraRaw = extra;
    }
  }
  if (reader.offset !== reader.end) {
    throw new Error(`Splitter ${guid} has ${reader.end - reader.offset} unread bytes.`);
  }
  return { version, children };
}

function buildExtra(panel) {
  if (panel.extraRaw) {
    return Buffer.concat([u32(panel.extraRaw.length), panel.extraRaw]);
  }
  const data = Buffer.concat([
    u32(panel.extraSize ?? panel.size ?? 150),
    u32(panel.extraDpi ?? 96),
  ]);
  return Buffer.concat([u32(data.length), data]);
}

function buildPanel(panel, splitterGuids) {
  const childData = panel.children
    ? buildSplitter(panel.guid, panel.children, splitterGuids)
    : panel.childData;
  return Buffer.concat([
    guidToBuffer(panel.guid),
    u32(panel.captionOrientation ?? 0),
    bool(panel.locked),
    bool(panel.hidden),
    bool(panel.showCaption),
    bool(panel.autohide),
    u32(panel.size ?? 150),
    bool(panel.showToggleArea),
    u32(childData.length),
    childData,
    bool(panel.useCustomTitle),
    string(panel.customTitle ?? ""),
  ]);
}

function buildSplitter(guid, splitter, splitterGuids) {
  if (!splitterGuids.has(guid)) throw new Error(`Not a splitter GUID: ${guid}`);
  return Buffer.concat([
    u32(splitter.version),
    u32(splitter.children.length),
    ...splitter.children.map((panel) => buildPanel(panel, splitterGuids)),
    ...splitter.children.map(buildExtra),
  ]);
}

function parseLayout(data, splitterGuids) {
  const reader = new Reader(data);
  const version = reader.u32();
  const active = reader.u32();
  const count = reader.u32();
  const presets = [];
  for (let index = 0; index < count; ++index) {
    const guid = reader.guid();
    const name = reader.string();
    const config = reader.bytes(reader.u32());
    presets.push({
      guid,
      name,
      config,
      children: parseSplitter(guid, config, splitterGuids),
    });
  }
  if (reader.offset !== reader.end) {
    throw new Error(`Layout has ${reader.end - reader.offset} unread bytes.`);
  }
  return { version, active, presets };
}

function buildLayout(layout, splitterGuids) {
  const presets = layout.presets.map((preset) => {
    const config = preset.children
      ? buildSplitter(preset.guid, preset.children, splitterGuids)
      : preset.config;
    return Buffer.concat([
      guidToBuffer(preset.guid),
      string(preset.name),
      u32(config.length),
      config,
    ]);
  });
  return Buffer.concat([
    u32(layout.version),
    u32(layout.active),
    u32(layout.presets.length),
    ...presets,
  ]);
}

function parseFcl(data) {
  const reader = new Reader(data);
  const header = reader.guid();
  const version = reader.u32();
  const mode = reader.u32();
  const panels = [];
  const panelCount = reader.u32();
  for (let index = 0; index < panelCount; ++index) {
    panels.push({ guid: reader.guid(), name: reader.string() });
  }
  const datasets = [];
  const datasetCount = reader.u32();
  for (let index = 0; index < datasetCount; ++index) {
    const guid = reader.guid();
    const name = reader.string();
    const required = [];
    const requiredCount = reader.u32();
    for (let item = 0; item < requiredCount; ++item) required.push(reader.u32());
    datasets.push({ guid, name, required, data: reader.bytes(reader.u32()) });
  }
  if (reader.offset !== reader.end) {
    throw new Error(`FCL has ${reader.end - reader.offset} unread bytes.`);
  }
  return { header, version, mode, panels, datasets };
}

function buildFcl(fcl) {
  const panels = [u32(fcl.panels.length)];
  for (const panel of fcl.panels) {
    panels.push(guidToBuffer(panel.guid), string(panel.name));
  }
  const datasets = [u32(fcl.datasets.length)];
  for (const dataset of fcl.datasets) {
    datasets.push(
      guidToBuffer(dataset.guid),
      string(dataset.name),
      u32(dataset.required.length),
      ...dataset.required.map(u32),
      u32(dataset.data.length),
      dataset.data);
  }
  return Buffer.concat([
    guidToBuffer(fcl.header),
    u32(fcl.version),
    u32(fcl.mode),
    ...panels,
    ...datasets,
  ]);
}

function pruneUnreferencedPanels(fcl) {
  const referenced = new Set();
  for (const dataset of fcl.datasets) {
    for (const index of dataset.required) {
      if (!Number.isInteger(index) || index < 0 || index >= fcl.panels.length) {
        throw new Error(`FCL dataset ${dataset.name} references invalid panel index ${index}.`);
      }
      referenced.add(index);
    }
  }

  const remap = new Map();
  const panels = [];
  for (let index = 0; index < fcl.panels.length; ++index) {
    if (!referenced.has(index)) continue;
    remap.set(index, panels.length);
    panels.push(fcl.panels[index]);
  }
  for (const dataset of fcl.datasets) {
    dataset.required = dataset.required.map((index) => remap.get(index));
  }

  const removed = fcl.panels.filter((_, index) => !referenced.has(index));
  fcl.panels = panels;
  return removed;
}

function makePanel(guid, size, dpi = 96) {
  return {
    guid,
    captionOrientation: 0,
    locked: false,
    hidden: false,
    showCaption: false,
    autohide: false,
    size,
    extraSize: size,
    extraDpi: dpi,
    showToggleArea: false,
    childData: Buffer.alloc(0),
    useCustomTitle: false,
    customTitle: "",
    children: null,
  };
}

module.exports = {
  buildFcl,
  buildLayout,
  makePanel,
  parseFcl,
  parseLayout,
  pruneUnreferencedPanels,
};
