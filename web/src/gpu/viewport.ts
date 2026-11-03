export type Viewport = [number, number, number, number, number, number];
/** Intersect the camera cell, letterboxed UI and normalized post-effect cell.
 * Normalized clips work on both full-size and downsampled bloom targets.
 * Fills and returns `out` (the encoder passes its own: a draw allocates nothing). */
export function drawScissor(v: ArrayLike<number>, width: number, height: number, clip = 0, fx0 = 0, fx1 = 0,
                            out: number[] = [0, 0, 0, 0]): number[] {
  const x0 = Math.max(Math.ceil(v[0]), fx1 ? Math.round((fx0 & 65535) * width / 65535) : 0);
  const x1 = Math.min(Math.floor(v[0] + v[2]), fx1 ? Math.round((fx1 & 65535) * width / 65535) : width);
  const y0 = Math.max(Math.ceil(v[1]), fx1 ? Math.round((fx0 >>> 16) * height / 65535) : 0, clip ? Math.min(clip & 65535, height) : 0);
  const y1 = Math.min(Math.floor(v[1] + v[3]), fx1 ? Math.round((fx1 >>> 16) * height / 65535) : height, clip ? Math.min(clip >>> 16, height) : height);
  out[0] = x0; out[1] = y0; out[2] = Math.max(0, x1 - x0); out[3] = Math.max(0, y1 - y0);
  return out;
}
/** Validate before setViewport: invalid guest state must not invalidate a
 * whole WebGPU frame. An unset viewport means the full render target.
 * Fills and returns `out`, or null for a viewport that is not valid. */
export function drawViewport(values: ArrayLike<number>, offset: number, width: number, height: number,
                             out: number[] = [0, 0, 0, 0, 0, 0]): number[] | null {
  if (!values[offset + 2]) { out[0] = 0; out[1] = 0; out[2] = width; out[3] = height; out[4] = 0; out[5] = 1; return out; }
  for (let i = 0; i < 6; i++) out[i] = values[offset + i];
  // written so that a NaN or an infinity fails it
  if (!(out[0] >= 0 && out[1] >= 0 && out[2] > 0 && out[3] > 0 && out[0] + out[2] <= width && out[1] + out[3] <= height
        && out[4] >= 0 && out[5] <= 1 && out[4] <= out[5])) return null;
  return out;
}
