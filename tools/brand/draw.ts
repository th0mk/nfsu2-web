// tools/brand/make.mjs bundles this into a page Chrome draws: every logo and icon file, as PNG data URLs, in the
// lettering of the mark on F12 pictures (web/src/gpu/mark.ts).
import { drawMark, markBox, markFont } from '../../web/src/gpu/mark';

const WHITE = '#ffffff', GREEN = '#b8de7d', BG = '#0d0d0d', FRAME = '#171717';
type G = CanvasRenderingContext2D;
type Part = [string, string];

function canvas(w: number, h: number): [HTMLCanvasElement, G] {
  const c = document.createElement('canvas'); c.width = w; c.height = h;
  return [c, c.getContext('2d')!];
}
function ink(g: G, text: string, px: number) {
  g.font = markFont(px);
  const m = g.measureText(text);
  return { l: m.actualBoundingBoxLeft, r: m.actualBoundingBoxRight, a: m.actualBoundingBoxAscent, d: m.actualBoundingBoxDescent };
}
function parts(g: G, ps: Part[], px: number, x: number, y: number) {
  g.font = markFont(px);
  for (const [t, c] of ps) { g.fillStyle = c; g.fillText(t, x, y); x += g.measureText(t).width; }
}
/** ps on one line, its ink w wide, its ink's middle at (cx, cy) */
function fit(g: G, ps: Part[], w: number, cx: number, cy: number) {
  const s = ps.map(p => p[0]).join(''), b0 = ink(g, s, 100), px = 100 * w / (b0.l + b0.r), b = ink(g, s, px);
  parts(g, ps, px, cx - (b.r - b.l) / 2, cy + (b.a - b.d) / 2);
}
/** "nfsu2" over ".xyz", their ink's left edges lined up, "nfsu2"'s ink w wide, the pair's ink in the middle at (cx, cy) */
function stacked(g: G, w: number, cx: number, cy: number) {
  const b0 = ink(g, 'nfsu2', 100), px = 100 * w / (b0.l + b0.r), a = ink(g, 'nfsu2', px), b = ink(g, '.xyz', px);
  const gap = px * 0.14, h = a.a + gap + b.a + b.d, y1 = cy - h / 2 + a.a, x0 = cx - (a.r + a.l) / 2 + a.l;
  parts(g, [['nfsu2', WHITE]], px, x0, y1);
  parts(g, [['.xyz', GREEN]], px, x0 - a.l + b.l, y1 + gap + b.a);
}
/** the start page's background: --bg with its soft light at the top (style.css body) */
function pageBg(g: G, w: number, h: number) {
  g.fillStyle = BG; g.fillRect(0, 0, w, h);
  const s = w / 1280;
  g.save(); g.translate(w / 2, -160 * s); g.scale(1, 480 / 1200); g.translate(-w / 2, 160 * s);
  const r = g.createRadialGradient(w / 2, -160 * s, 0, w / 2, -160 * s, 1200 * s * 0.7);
  r.addColorStop(0, '#2a2a2a'); r.addColorStop(1, 'rgba(42,42,42,0)');
  g.fillStyle = r; g.fillRect(0, -2000, w, 6000); g.restore();
}
function rounded(g: G, n: number, colour: string) {
  g.beginPath(); g.roundRect(0, 0, n, n, n / 4); g.fillStyle = colour; g.fill();
}
/** the canvas cut to what is drawn on it (alpha > 0), a pixel round it */
function trim(c: HTMLCanvasElement): HTMLCanvasElement {
  const d = c.getContext('2d')!.getImageData(0, 0, c.width, c.height).data;
  let x0 = c.width, y0 = c.height, x1 = -1, y1 = -1;
  for (let y = 0; y < c.height; y++) for (let x = 0; x < c.width; x++)
    if (d[(y * c.width + x) * 4 + 3]) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
  const [o, g] = canvas(x1 - x0 + 3, y1 - y0 + 3);
  g.drawImage(c, x0 - 1, y0 - 1, o.width, o.height, 0, 0, o.width, o.height);
  return o;
}
/** the logo: the mark at size px on a clear background, with its shadow (or in other colours, without) */
function logo(px: number, dark?: [string, string]) {
  const [c, g] = canvas(px * 6, px * 3);
  if (dark) parts(g, [['nfsu2', dark[0]], ['.xyz', dark[1]]], px, px, px * 1.5);
  else drawMark(g, px, px, px * 1.5);
  return trim(c);
}

export function brand(): [string, string][] {
  const out: [string, string][] = [];
  const put = (name: string, c: HTMLCanvasElement) => out.push([name, c.toDataURL('image/png')]);

  put('tools/brand/logo.png', logo(400));
  put('tools/brand/logo-dark.png', logo(400, [FRAME, '#5f8a1e']));
  { const [c, g] = canvas(1024, 1024); g.fillStyle = BG; g.fillRect(0, 0, 1024, 1024); stacked(g, 640, 512, 512); put('tools/brand/discord-icon.png', c); }
  // Discord's channel list shows only a band of the banner across its middle, about 2.4:1 (the rest is cut): the mark
  // goes in that band's bottom right, as on a screenshot, with room for a band down to 2.6:1 (y 171..909)
  { const [c, g] = canvas(1920, 1080); pageBg(g, 1920, 1080);
    const px = 150, m = 110; drawMark(g, px, 1920 - m - markBox(g, px).w, 800); put('tools/brand/discord-banner.png', c); }
  for (const n of [16, 32]) {
    const [c, g] = canvas(n, n); rounded(g, n, FRAME); fit(g, [['u2', WHITE]], n * 0.74, n / 2, n / 2);
    put(`web/src/static/favicon-${n}.png`, c);
  }
  // the home screen's: an iPhone rounds its own (and fills a clear corner black), Android shows "any" as it is and
  // cuts "maskable" to its shape (the safe zone: a circle 80 % across, which the two lines keep inside)
  for (const [name, n, round] of [['icon-180', 180, false], ['icon-192', 192, true], ['icon-512', 512, true], ['icon-maskable-512', 512, false]] as const) {
    const [c, g] = canvas(n, n);
    if (round) rounded(g, n, BG); else { g.fillStyle = BG; g.fillRect(0, 0, n, n); }
    stacked(g, n * 0.625, n / 2, n / 2);
    put(`web/src/static/${name}.png`, c);
  }
  return out;
}
(window as unknown as { brand: typeof brand }).brand = brand;
