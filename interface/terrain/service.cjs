/* Authoritative placement calculations over the vendored DEM; JSON lines in/out, no network or npm. */
const fs = require('node:fs');
const zlib = require('node:zlib');
const readline = require('node:readline');
const LL = require('./model.cjs');
const meta = require('./meta.json');
const t = meta.terrain;
const packed = zlib.gunzipSync(fs.readFileSync(__dirname + '/dem20.gz'));
const bytes = new Uint8Array(packed.length), count = packed.length / 4;
if (count !== t.n * t.n) throw new Error('Wrong DEM dimensions');
for (let k = 0; k < 4; k++) for (let i = 0; i < count; i++) bytes[i * 4 + k] = packed[k * count + i];
const terrain = new LL.Terrain(new Float32Array(bytes.buffer), t.n, t.n, t.x0, t.ytop, t.cell, t.bx, t.by, t.rdisk);
const params = new LL.Params(433e6);
const clamp = (x, lo, hi) => Math.max(lo, Math.min(hi, x));
function signal(a, b, settings) {
  const result = LL.linkLoss(terrain, a.x, a.y, b.x, b.y, a.h, b.h, params, false);
  if (!Number.isFinite(result.L)) throw new Error('Invalid terrain path');
  const raw = settings.power - result.L - settings.attenuation, margin = raw + 129;
  const per = margin > 20 ? 0 : 1 / (1 + 9 * Math.exp(margin / 0.5));
  return { margin, loss: Math.round(per * 100), rssi: Math.round(clamp(raw, -200, 30)),
    snr: Math.round(clamp(raw + 117.03, -30, 12)), distance: result.D, los: !!result.los };
}
function layout(nodes, settings) {
  const pairs = [];
  for (let i = 0; i < nodes.length; i++) for (let j = i + 1; j < nodes.length; j++) {
    const ab = signal(nodes[i], nodes[j], settings), ba = signal(nodes[j], nodes[i], settings);
    pairs.push({ a: nodes[i].id, b: nodes[j].id, ab, ba });
  }
  return { pairs };
}
// Lossless PNG hillshade from the actual elevation grid, generated once per service.
function crc32(buffer) {
  let crc = 0xffffffff;
  for (const byte of buffer) { crc ^= byte; for (let k = 0; k < 8; k++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0); }
  return (crc ^ 0xffffffff) >>> 0;
}
function chunk(name, data) {
  const body = Buffer.concat([Buffer.from(name), data]), header = Buffer.alloc(4), crc = Buffer.alloc(4);
  header.writeUInt32BE(data.length); crc.writeUInt32BE(crc32(body)); return Buffer.concat([header, body, crc]);
}
let hillshade;
function mapImage() {
  if (hillshade) return hillshade;
  const n = 1024, row = n * 4 + 1, data = Buffer.alloc(row * n), extent = (t.n - 1) * t.cell;
  for (let r = 0; r < n; r++) for (let c = 0; c < n; c++) {
    const x = t.x0 + c / (n - 1) * extent, y = t.ytop - r / (n - 1) * extent;
    const dx = (terrain.sample(x + 20, y) - terrain.sample(x - 20, y)) / 40;
    const dy = (terrain.sample(x, y + 20) - terrain.sample(x, y - 20)) / 40;
    const shade = clamp((0.7 - 0.5 * dx + 0.45 * dy) / Math.sqrt(1 + dx * dx + dy * dy), 0.08, 1);
    const v = 35 + 150 * shade, off = r * row + c * 4 + 1;
    data[off] = v * 0.94; data[off + 1] = v; data[off + 2] = v * 1.06;
    data[off + 3] = Math.hypot(x - t.bx, y - t.by) < t.rdisk ? 255 : 0;
  }
  const header = Buffer.alloc(13); header.writeUInt32BE(n); header.writeUInt32BE(n, 4); header[8] = 8; header[9] = 6;
  hillshade = Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]), chunk('IHDR', header), chunk('IDAT', zlib.deflateSync(data)), chunk('IEND', Buffer.alloc(0))]).toString('base64');
  return hillshade;
}
function handle(req) {
  if (req.op === 'meta') return meta;
  if (req.op === 'map') return { png: mapImage() };
  if (req.op === 'layout') return layout(req.nodes, req.settings);
  if (req.op === 'coverage') {
    const n = 60, values = [], b = req.bounds;
    for (let r = 0; r < n; r++) for (let c = 0; c < n; c++) {
      const x = b.x + (c + 0.5) / n * b.width, y = b.y - (r + 0.5) / n * b.height;
      values.push(Math.hypot(x - t.bx, y - t.by) > t.rdisk - 20 ? null : Number(signal(req.node, { x, y, h: 1.5 }, req.settings).margin.toFixed(2)));
    }
    return { size: n, values };
  }
  throw new Error('Unknown terrain calculation');
}
readline.createInterface({ input: process.stdin }).on('line', line => {
  try { console.log(JSON.stringify({ ok: true, ...handle(JSON.parse(line)) })); }
  catch (error) { console.log(JSON.stringify({ ok: false, reason: error.message })); }
});
