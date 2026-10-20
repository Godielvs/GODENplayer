// Optional design-time tool. Runtime/player builds use the checked-in assets.
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const sharp = require('sharp');

const root = path.resolve(__dirname, '../..');
const sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256];
const hash = b => crypto.createHash('sha256').update(b).digest('hex');

async function main() {
  if (sharp.versions.sharp !== '0.35.5') throw new Error('Use the pinned icon tooling: npm --prefix tools/icon-assets ci --ignore-scripts');
  const args = process.argv.slice(2);
  if (args.length !== 4) throw new Error('Expected: ICO PNG PREVIEW MANIFEST (use empty string to skip optional outputs)');
  const [icoFile, pngFile, previewFile, manifestFile] = args;
  const source = await fs.readFile(path.join(root, 'packaging/brand/goden.svg'));
  // Keep rasterization bounded and repeatable; no runtime service or worker.
  sharp.concurrency(1);
  sharp.cache(false);
  const png = size => sharp(source, { density: 96, limitInputPixels: 1024 * 1024 })
    .resize(size, size, { fit: 'contain' }).png({ compressionLevel: 9, adaptiveFiltering: false }).toBuffer();
  const layers = [];
  for (const size of sizes) layers.push(await png(size));
  const header = Buffer.alloc(6 + 16 * sizes.length);
  header.writeUInt16LE(1, 2);
  header.writeUInt16LE(sizes.length, 4);
  let offset = header.length;
  const records = [];
  for (let i = 0; i < sizes.length; i++) {
    const entry = 6 + 16 * i;
    header[entry] = header[entry + 1] = sizes[i] === 256 ? 0 : sizes[i];
    header.writeUInt16LE(1, entry + 4);
    header.writeUInt16LE(32, entry + 6);
    header.writeUInt32LE(layers[i].length, entry + 8);
    header.writeUInt32LE(offset, entry + 12);
    records.push({ size: sizes[i], bytes: layers[i].length, sha256: hash(layers[i]) });
    offset += layers[i].length;
  }
  const outputs = [];
  async function save(file, bytes, label) {
    if (!file) return;
    await fs.mkdir(path.dirname(path.resolve(file)), { recursive: true });
    await fs.writeFile(file, bytes);
    outputs.push({ file: label, bytes: bytes.length, sha256: hash(bytes) });
  }
  await save(icoFile, Buffer.concat([header, ...layers]), 'goden.ico');
  if (pngFile) await save(pngFile, await png(1024), 'goden-presence-1024.png');
  if (previewFile) {
    const chosen = [16, 24, 32, 48, 128, 256];
    const overlays = [];
    for (let i = 0; i < chosen.length; i++) {
      const size = chosen[i];
      const shown = Math.min(128, size * 4);
      const image = await sharp(layers[sizes.indexOf(size)]).resize(shown, shown).png().toBuffer();
      overlays.push({ input: image, left: i * 168 + Math.floor((168 - shown) / 2), top: Math.floor((176 - shown) / 2) });
    }
    const preview = await sharp({ create: { width: chosen.length * 168, height: 176, channels: 4, background: '#16191a' } })
      .composite(overlays).png().toBuffer();
    await save(previewFile, preview, 'preview.png');
  }
  if (manifestFile) {
    const manifest = { schema: 'goden.icon-assets.v1', design: 'B / Angular',
      source: 'packaging/brand/goden.svg', sourceSha256: hash(source),
      renderer: { sharp: sharp.versions.sharp, vips: sharp.versions.vips }, layers: records, outputs };
    await fs.mkdir(path.dirname(path.resolve(manifestFile)), { recursive: true });
    await fs.writeFile(manifestFile, JSON.stringify(manifest, null, 2) + '\n');
  }
  console.log('GODEN B icon: ' + sizes.length + ' ICO layers' + (pngFile ? ' and 1024 px Presence PNG' : ''));
}
main().catch(error => { console.error(error.message); process.exitCode = 1; });
