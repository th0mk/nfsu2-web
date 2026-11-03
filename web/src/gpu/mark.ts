// The site's address in the look of the game's menus: bold italic, "nfsu2" white, ".xyz" in their green, with a soft
// shadow.  F12 pictures have it in their corner (png-worker.ts), the title screen under EA's copyright
// (app/titlemark.ts), and the logo files are drawn with it (tools/brand).

type Ctx = CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D;

export const markFont = (px: number) => `italic 700 ${px}px "Helvetica Neue", Helvetica, Arial, sans-serif`;

/** the address's ink at size px, about the start of its baseline: l left of it, r right of it, a above it, d under it */
export function markBox(g: Ctx, px: number) {
  g.font = markFont(px);
  const m = g.measureText('nfsu2.xyz');
  return { w: m.width, l: m.actualBoundingBoxLeft, r: m.actualBoundingBoxRight, a: m.actualBoundingBoxAscent, d: m.actualBoundingBoxDescent };
}

/** the address at size px, its baseline starting at (x, y), its shadow in the margin round it (0.6 px enough) */
export function drawMark(g: Ctx, px: number, x: number, y: number) {
  g.save();
  g.font = markFont(px);
  g.shadowColor = 'rgba(0,0,0,0.65)'; g.shadowBlur = px * 0.3; g.shadowOffsetY = Math.max(1, px * 0.06);
  g.fillStyle = 'rgba(255,255,255,0.92)'; g.fillText('nfsu2', x, y);
  g.fillStyle = '#b8de7d'; g.fillText('.xyz', x + g.measureText('nfsu2').width, y);
  g.restore();
}
