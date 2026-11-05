// The flags before players' names over their cars (runtime/roam_game.c, flag_*): flag-icons' (github.com/lipis/flag-icons,
// MIT; licenses/flag-icons.txt) 4:3 SVGs drawn at 48x36, as runtime/flags.dat.
//
//   npm pack flag-icons@7.6.0 && tar -xzf flag-icons-7.6.0.tgz    (anywhere: its package/ folder)
//   curl -o <more folder>/ca-qc.svg https://upload.wikimedia.org/wikipedia/commons/5/5f/Flag_of_Quebec.svg
//   node web/tools/mkflags.mjs <package folder> [runtime/flags.dat] --more <more folder>
// (Playwright's Chrome draws them: channel 'chrome')
//
// The flags: every country of ISO 3166-1 (flag-icons' iso: true), Kosovo (XK, as Cloudflare names its country), the
// United Kingdom's four nations (gb-eng, gb-sct, gb-wls, gb-nir) and Quebec (ca-qc): web/src/app/flags.ts FLAG_CODES,
// the same list in the same order, which is the codes' (the game looks one up by halves).  A flag flag-icons does not
// have comes from the --more folder as <code>.svg: Quebec's is Wikimedia Commons' "Flag of Quebec.svg" (public
// domain), 3:2, drawn at 4:3 as it comes (a little narrower; a crop put its fleurs-de-lis off their quarters' middle
// and the cross's edges between pixels).  Each one is drawn 4x as large and averaged down (the edges and the small
// emblems soft, as the names' letters are), with no alpha: the game makes its texture of it (64x64, the rounded
// corners its own).  At 48x36 the 255 are 230 KB in the module, 109 KB of its download (brotli); at 64x64 they were
// 426 KB and 185 KB.
//
// flags.dat, little-endian: "FLG1", u16 count, u8 width (48), u8 height (36); count entries of { char code[8]
// (zero-padded), u32 offset, u32 length } (offsets from the file's start); then each flag's pixels row by row, red,
// green, blue, in runs: a byte n < 128 is n + 1 pixels as they come (3 bytes each), n >= 128 is one pixel n - 126
// times (2..129).
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const argv = process.argv.slice(2), mi = argv.indexOf('--more');
const more = mi >= 0 ? argv.splice(mi, 2)[1] : null;
const [pkg, out = path.join(REPO, 'runtime/flags.dat')] = argv;
if (!pkg || mi >= 0 && !more) { console.error('usage: node web/tools/mkflags.mjs <flag-icons package folder> [out] [--more <folder of other flags>]'); process.exit(2); }
const src = fs.readFileSync(path.join(REPO, 'web/src/app/flags.ts'), 'utf8');
const m = src.match(/FLAG_CODES\s*=\s*\[([^\]]*)\]/);
if (!m) throw new Error('web/src/app/flags.ts: no FLAG_CODES');
const codes = [...m[1].matchAll(/'([a-z-]+)'/g)].map(x => x[1]);
for (let i = 1; i < codes.length; i++) if (!(codes[i - 1] < codes[i])) throw new Error(`FLAG_CODES: ${codes[i - 1]} before ${codes[i]}`);
const W = 48, H = 36, SS = 4;

const b = await chromium.launch({ channel: 'chrome' });
const page = await b.newPage();
const flags = [];
for (const code of codes) {
  // flag-icons' 4:3 one, else the --more folder's
  const file = [path.join(pkg, 'flags/4x3', `${code}.svg`), more && path.join(more, `${code}.svg`)].find(f => f && fs.existsSync(f));
  if (!file) throw new Error(`no flag for ${code}: not in ${path.join(pkg, 'flags/4x3')}${more ? ` or ${more}` : ' (and no --more folder)'}`);
  const svg = 'data:image/svg+xml;base64,' + fs.readFileSync(file).toString('base64');
  const rgb = await page.evaluate(async ({ svg, W, H, SS }) => {
    const im = new Image(); im.src = svg; await im.decode();
    const w = W * SS, h = H * SS, c = new OffscreenCanvas(w, h), g = c.getContext('2d');
    g.fillStyle = '#000'; g.fillRect(0, 0, w, h);
    g.drawImage(im, 0, 0, w, h);
    const d = g.getImageData(0, 0, w, h).data, o = [];
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) for (let k = 0; k < 3; k++) {
      let s = 0;
      for (let v = 0; v < SS; v++) for (let u = 0; u < SS; u++) s += d[4 * ((y * SS + v) * w + x * SS + u) + k];
      o.push(Math.round(s / (SS * SS)));
    }
    return o;
  }, { svg, W, H, SS });
  flags.push({ code, data: runs(Uint8Array.from(rgb)) });
}
await b.close();

/** a flag's pixels in runs (see the top) */
function runs(px) {
  const n = px.length / 3, o = [], same = (i, j) => px[3 * i] === px[3 * j] && px[3 * i + 1] === px[3 * j + 1] && px[3 * i + 2] === px[3 * j + 2];
  let i = 0;
  while (i < n) {
    let r = 1;
    while (i + r < n && r < 129 && same(i, i + r)) r++;
    if (r >= 2) { o.push(126 + r, px[3 * i], px[3 * i + 1], px[3 * i + 2]); i += r; continue; }
    let l = 1;
    while (i + l < n && l < 128 && !(i + l + 1 < n && same(i + l, i + l + 1))) l++;
    o.push(l - 1);
    for (let k = 0; k < l; k++) o.push(px[3 * (i + k)], px[3 * (i + k) + 1], px[3 * (i + k) + 2]);
    i += l;
  }
  return Uint8Array.from(o);
}

const head = 8 + 16 * flags.length, total = head + flags.reduce((a, f) => a + f.data.length, 0);
const buf = Buffer.alloc(total);
buf.write('FLG1', 0, 'ascii'); buf.writeUInt16LE(flags.length, 4); buf.writeUInt8(W, 6); buf.writeUInt8(H, 7);
let at = head;
flags.forEach((f, i) => {
  buf.write(f.code, 8 + 16 * i, 'ascii');
  buf.writeUInt32LE(at, 8 + 16 * i + 8); buf.writeUInt32LE(f.data.length, 8 + 16 * i + 12);
  buf.set(f.data, at); at += f.data.length;
});
fs.writeFileSync(out, buf);
console.log(`${out}: ${flags.length} flags, ${total} bytes`);
