// Optional documentation tooling, using the same pinned renderer as the icon.
// Composes existing project assets; does not change the player or upload files.
const fs = require('node:fs/promises');
const path = require('node:path');
const crypto = require('node:crypto');
const sharp = require('sharp');

async function main() {
  if (sharp.versions.sharp !== '0.35.5') throw new Error('Use npm --prefix tools/icon-assets ci --ignore-scripts');
  sharp.concurrency(1);
  sharp.cache(false);
  const root = path.resolve(__dirname, '../..');
  const sourcePath = path.join(root, 'docs/media/source/social-preview.svg');
  let svg = await fs.readFile(sourcePath, 'utf8');
  const inputs = [sourcePath, path.join(root, 'assets/pixel-ui/brand/wordmark_standard.svg')];
  for (const [relative, mime] of [['../../../packaging/brand/goden.svg', 'image/svg+xml'], ['../album.png', 'image/png']]) {
    const input = path.resolve(path.dirname(sourcePath), relative);
    inputs.push(input);
    svg = svg.replaceAll(`href="${relative}"`, `href="data:${mime};base64,${(await fs.readFile(input)).toString('base64')}"`);
  }
  const output = path.join(root, 'docs/media/social-preview.png');
  const png = await sharp(Buffer.from(svg), {limitInputPixels: 1280 * 640})
    .png({compressionLevel: 9}).toBuffer();
  const info = await sharp(png).metadata();
  if (info.width !== 1280 || info.height !== 640 || png.length >= 1000000) throw new Error('Social preview must be 1280 x 640 and under 1 MB');
  await fs.writeFile(output, png);
  const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
  const manifest = {design: 'Brand and player, native pixel wordmark', width: info.width, height: info.height, bytes: png.length,
    sha256: hash(png), renderer: {sharp: sharp.versions.sharp, vips: sharp.versions.vips}, inputs: []};
  for (const input of inputs) manifest.inputs.push({file: path.relative(root, input).replaceAll('\\', '/'), sha256: hash(await fs.readFile(input))});
  await fs.writeFile(path.join(root, 'docs/media/source/social-preview.json'), JSON.stringify(manifest, null, 2) + '\n');
  console.log(`Social preview: ${info.width} x ${info.height}, ${png.length} bytes`);
}
main().catch(error => {console.error(error.message); process.exitCode = 1;});
