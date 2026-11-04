// The renderer's frame dumps (index.ts readback: F12 screenshots, the tests'
// dumps) made into PNGs here instead of on the page thread, which draws the
// game: converting and encoding a 4K frame held it for over 100 ms.
// In: a readback's bytes (BGRA rows bpr apart); out: the frame as a PNG,
// scaled down by `scale` when it is below 1, with the site's address in its
// corner when `mark` is set (F12 pictures, unless photo mode's panel turned it off).
// A picture bigger than the screen comes as tiles (photo mode's Size): each put in its place of one big picture as
// it comes, then the crop's rectangle of it made into the PNG.

import { drawMark, markBox } from './mark';

export interface ToPng { id: number; px: ArrayBuffer; w: number; h: number; bpr: number; scale: number; mark?: boolean }
/** a tile of picture `pic` (W x H in all) at x, y; its last message (end) gives the crop's rectangle and the mark */
export interface ToPngTile { tile: true; pic: number; px: ArrayBuffer; w: number; h: number; bpr: number; x: number; y: number; W: number; H: number }
export interface ToPngEnd { end: true; id: number; pic: number; crop: [number, number, number, number]; mark?: boolean }
export type FromPng = { id: number; png: Blob } | { id: number; error: string };

const post = (m: FromPng) => (self as unknown as Worker).postMessage(m);

/** the address bottom right (mark.ts), 2.6 % of the picture's height */
function mark(g: OffscreenCanvasRenderingContext2D, w: number, h: number) {
  const px = Math.max(12, Math.round(h * 0.026)), m = Math.round(h * 0.024);
  drawMark(g, px, w - m - markBox(g, px).w, h - m);
}

/** a readback's BGRA rows as an image */
function image(px: ArrayBuffer, w: number, h: number, bpr: number): ImageData {
  const src = new Uint8Array(px), img = new ImageData(w, h), d = img.data;
  for (let y = 0; y < h; y++) {
    let s = y * bpr, o = y * w * 4;
    for (let x = 0; x < w; x++, s += 4, o += 4) { d[o] = src[s + 2]; d[o + 1] = src[s + 1]; d[o + 2] = src[s]; d[o + 3] = 255; }
  }
  return img;
}

// the big pictures being put together, by number
const big = new Map<number, OffscreenCanvas>();

onmessage = async (e: MessageEvent<ToPng | ToPngTile | ToPngEnd>) => {
  const m = e.data;
  if ('tile' in m) {
    let c = big.get(m.pic);
    if (!c) big.set(m.pic, c = new OffscreenCanvas(m.W, m.H));
    c.getContext('2d')!.putImageData(image(m.px, m.w, m.h, m.bpr), m.x, m.y);
    return;
  }
  if ('end' in m) {
    const c = big.get(m.pic);
    big.delete(m.pic);
    try {
      if (!c) throw new Error('no tiles');
      const [x, y, w, h] = m.crop;
      let oc = c;
      if (x || y || w !== c.width || h !== c.height) { oc = new OffscreenCanvas(w, h); oc.getContext('2d')!.drawImage(c, -x, -y); }
      if (m.mark) mark(oc.getContext('2d')!, oc.width, oc.height);
      post({ id: m.id, png: await oc.convertToBlob({ type: 'image/png' }) });
    } catch (x) { post({ id: m.id, error: String(x) }); }
    return;
  }
  const { id, px, w, h, bpr, scale, mark: marked } = m;
  try {
    const img = image(px, w, h, bpr);
    let oc = new OffscreenCanvas(w, h);
    oc.getContext('2d')!.putImageData(img, 0, 0);
    if (scale > 0 && scale < 1) {
      const o2 = new OffscreenCanvas(Math.max(1, Math.round(w * scale)), Math.max(1, Math.round(h * scale)));
      const g = o2.getContext('2d')!;
      g.imageSmoothingQuality = 'high';
      g.drawImage(oc, 0, 0, o2.width, o2.height);
      oc = o2;
    }
    if (marked) mark(oc.getContext('2d')!, oc.width, oc.height);
    post({ id, png: await oc.convertToBlob({ type: 'image/png' }) });
  } catch (x) { post({ id, error: String(x) }); }
};
