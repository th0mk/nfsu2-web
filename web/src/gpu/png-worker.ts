// The renderer's frame dumps (index.ts readback: F12 screenshots, the tests'
// dumps) made into PNGs here instead of on the page thread, which draws the
// game: converting and encoding a 4K frame held it for over 100 ms.
// In: a readback's bytes (BGRA rows bpr apart); out: the frame as a PNG,
// scaled down by `scale` when it is below 1, with the site's address in its
// corner when `mark` is set (F12 pictures, unless photo mode's panel turned it off).

export interface ToPng { id: number; px: ArrayBuffer; w: number; h: number; bpr: number; scale: number; mark?: boolean }
export type FromPng = { id: number; png: Blob } | { id: number; error: string };

const post = (m: FromPng) => (self as unknown as Worker).postMessage(m);

/** the address bottom right, bold italic as the game's menus, ".xyz" in their green; 2.6 % of the picture's height */
function mark(g: OffscreenCanvasRenderingContext2D, w: number, h: number) {
  const px = Math.max(12, Math.round(h * 0.026)), m = Math.round(h * 0.024), y = h - m;
  g.font = `italic 700 ${px}px "Helvetica Neue", Helvetica, Arial, sans-serif`;
  g.shadowColor = 'rgba(0,0,0,0.65)'; g.shadowBlur = px * 0.3; g.shadowOffsetY = Math.max(1, px * 0.06);
  const x = w - m - g.measureText('nfsu2.xyz').width;
  g.fillStyle = 'rgba(255,255,255,0.92)'; g.fillText('nfsu2', x, y);
  g.fillStyle = '#b8de7d'; g.fillText('.xyz', x + g.measureText('nfsu2').width, y);
}

onmessage = async (e: MessageEvent<ToPng>) => {
  const { id, px, w, h, bpr, scale, mark: marked } = e.data;
  try {
    const src = new Uint8Array(px), img = new ImageData(w, h), d = img.data;
    for (let y = 0; y < h; y++) {
      let s = y * bpr, o = y * w * 4;
      for (let x = 0; x < w; x++, s += 4, o += 4) { d[o] = src[s + 2]; d[o + 1] = src[s + 1]; d[o + 2] = src[s]; d[o + 3] = 255; }
    }
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
