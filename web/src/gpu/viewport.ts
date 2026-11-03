export type Viewport = [number, number, number, number, number, number];
/** Intersect the camera cell, letterboxed UI and normalized post-effect cell.
 * Normalized clips work on both full-size and downsampled bloom targets. */
export function drawScissor(v: Viewport, width: number, height: number, clip = 0, fx0 = 0, fx1 = 0): [number, number, number, number] {
  const x0 = Math.max(Math.ceil(v[0]), fx1 ? Math.round((fx0 & 65535) * width / 65535) : 0);
  const x1 = Math.min(Math.floor(v[0] + v[2]), fx1 ? Math.round((fx1 & 65535) * width / 65535) : width);
  const y0 = Math.max(Math.ceil(v[1]), fx1 ? Math.round((fx0 >>> 16) * height / 65535) : 0, clip ? Math.min(clip & 65535, height) : 0);
  const y1 = Math.min(Math.floor(v[1] + v[3]), fx1 ? Math.round((fx1 >>> 16) * height / 65535) : height, clip ? Math.min(clip >>> 16, height) : height);
  return [x0, y0, Math.max(0, x1 - x0), Math.max(0, y1 - y0)];
}
/** Validate before setViewport: invalid guest state must not invalidate a
 * whole WebGPU frame. An unset viewport means the full render target. */
export function drawViewport(values: ArrayLike<number>, offset: number, width: number, height: number): Viewport | null {
  if (!values[offset + 2]) return [0, 0, width, height, 0, 1];
  const v: Viewport = [values[offset], values[offset + 1], values[offset + 2], values[offset + 3], values[offset + 4], values[offset + 5]];
  if (v.some(n => !Number.isFinite(n)) || v[0] < 0 || v[1] < 0 || v[2] <= 0 || v[3] <= 0 ||
      v[0] + v[2] > width || v[1] + v[3] > height || v[4] < 0 || v[5] > 1 || v[4] > v[5]) return null;
  return v;
}
