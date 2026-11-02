// Photo mode's panel (photo.ts), the parts without the page: the focus slider's scale, the filters' looks, the
// crops and the part of the picture each keeps.  (web/test/photolook.test.mjs)
import type { PhotoLook } from '../gpu/photo-picture';

/** the focus slider (0 .. 1000): FOCUS_MIN m to FOCUS_MAX m, evenly in their ratio */
export const FOCUS_MIN = 0.5, FOCUS_MAX = 500;
export const focusM = (v: number) => FOCUS_MIN * (FOCUS_MAX / FOCUS_MIN) ** (v / 1000);
export const focusV = (m: number) =>
  Math.round(Math.log(Math.max(FOCUS_MIN, Math.min(FOCUS_MAX, m)) / FOCUS_MIN) / Math.log(FOCUS_MAX / FOCUS_MIN) * 1000);

/** the filters: their look over the picture */
export type Filter = 'none' | 'bw' | 'warm' | 'cool' | 'vivid' | 'faded';
export const FILTERS: Record<Filter, Pick<PhotoLook, 'contrast' | 'saturation' | 'tint' | 'fade'>> = {
  none: { contrast: 1, saturation: 1, tint: [1, 1, 1], fade: 0 },
  bw: { contrast: 1.12, saturation: 0, tint: [1, 1, 1], fade: 0 },
  warm: { contrast: 1.04, saturation: 1.08, tint: [1.08, 1, 0.86], fade: 0 },
  cool: { contrast: 1.04, saturation: 0.95, tint: [0.88, 0.98, 1.1], fade: 0 },
  vivid: { contrast: 1.12, saturation: 1.4, tint: [1, 1, 1], fade: 0 },
  faded: { contrast: 0.88, saturation: 0.7, tint: [1.02, 1, 0.96], fade: 0.07 },
};
/** the crops: width over height (0: the whole picture) */
export const CROPS: Record<string, number> = { full: 0, '16:9': 16 / 9, '21:9': 21 / 9, '3:2': 3 / 2, '4:3': 4 / 3, '1:1': 1, '4:5': 4 / 5, '9:16': 9 / 16 };
/** the part of a picture w / h wide that a crop of width over height r keeps, the middle of it: left, top, right, bottom */
export function cropRect(r: number, aspect: number): [number, number, number, number] {
  if (!(r > 0) || !(aspect > 0) || Math.abs(r - aspect) < 1e-3) return [0, 0, 1, 1];
  if (r > aspect) { const h = aspect / r; return [0, (1 - h) / 2, 1, (1 + h) / 2]; }
  const w = r / aspect;
  return [(1 - w) / 2, 0, (1 + w) / 2, 1];
}
