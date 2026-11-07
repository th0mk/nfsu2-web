// The frame-rate counter over the game (F8: off, on, detailed).  The game's
// own rate comes from its presented-frame counter (HOST_KIND_STATUS, see
// status.ts); until a status block exists it falls back to the page's
// requestAnimationFrame rate, and says so.

import type { StatusReader } from './status';
import type { GameMemory } from '../host/memory';

export type FpsMode = 'off' | 'on' | 'detailed';
export const FPS_MODES: FpsMode[] = ['off', 'on', 'detailed'];

export class FpsOverlay {
  mode: FpsMode = 'off';
  private raf = 0;
  private rafTimes: number[] = [];
  private samples: { t: number; frames: number; tris: number }[] = [];
  private lastPaint = 0;
  reader: StatusReader | null = null;
  extra: () => Record<string, number> = () => ({});
  memory: GameMemory | null = null;
  /** latest game frame rate (for tests and the page) */
  gameFps = 0;
  pageFps = 0;

  constructor(private el: HTMLElement) {}

  set(mode: FpsMode) {
    this.mode = mode;
    this.el.hidden = mode === 'off';
    if (mode !== 'off' && !this.raf) this.raf = requestAnimationFrame(this.tick);
    if (mode === 'off' && this.raf) { cancelAnimationFrame(this.raf); this.raf = 0; }
  }
  cycle(): FpsMode { const m = FPS_MODES[(FPS_MODES.indexOf(this.mode) + 1) % FPS_MODES.length]; this.set(m); return m; }
  stop() { if (this.raf) cancelAnimationFrame(this.raf); this.raf = 0; this.samples = []; this.rafTimes = []; this.el.hidden = true; }

  private tick = (now: number) => {
    this.raf = requestAnimationFrame(this.tick);
    this.rafTimes.push(now);
    while (this.rafTimes.length > 2 && now - this.rafTimes[0] > 1000) this.rafTimes.shift();
    if (now - this.lastPaint < 250) return;
    this.lastPaint = now;
    const rt = this.rafTimes;
    this.pageFps = rt.length > 1 ? (rt.length - 1) * 1000 / (rt[rt.length - 1] - rt[0]) : 0;
    const st = this.reader?.read() ?? null;
    if (st?.has('frames')) {
      // the game's own clock: frames_t is when the C side read frames (the page's
      // sampling times do not line up with its updates); a block without it
      // (an older build) falls back to the page's time
      const t = st.get('frames_t') ?? now, f = st.get('frames')!;
      const last = this.samples[this.samples.length - 1];
      if (!last || t !== last.t) this.samples.push({ t, frames: f, tris: st.get('tris') ?? 0 });
      while (this.samples.length > 2 && t - this.samples[0].t > 1000) this.samples.shift();
      const a = this.samples[0], b = this.samples[this.samples.length - 1];
      if (b.t > a.t && b.frames >= a.frames) this.gameFps = (b.frames - a.frames) * 1000 / (b.t - a.t);
      else if (this.samples.length < 2) this.gameFps = 0;
    }
    this.paint(st);
  };

  private paint(st: Map<string, number> | null) {
    const game = st?.has('frames');
    const fps = game ? this.gameFps : this.pageFps;
    const big = document.createElement('div');
    big.textContent = String(Math.round(fps));
    const small = document.createElement('small'); small.textContent = game ? 'fps' : 'fps page';
    big.append(small);
    const parts: Node[] = [big];
    if (this.mode === 'detailed') {
      const dl = document.createElement('dl');
      const add = (k: string, v: string) => { const dt = document.createElement('dt'); dt.textContent = k; const dd = document.createElement('dd'); dd.textContent = v; dl.append(dt, dd); };
      if (game && this.gameFps > 0) add('frame', `${(1000 / this.gameFps).toFixed(2)} ms`);
      add('display', `${this.pageFps.toFixed(0)} Hz`);
      if (game && this.samples.length > 1) {
        const a = this.samples[0], b = this.samples[this.samples.length - 1];
        const df = b.frames - a.frames;
        if (df > 0 && b.tris >= a.tris) add('tris/frame', Math.round((b.tris - a.tris) / df).toLocaleString('en-US'));
      }
      if (st) for (const [k, v] of st) if (k !== 'frames' && k !== 'frames_t' && k !== 'tris') add(k.replace(/_/g, ' '), fmt(v));
      for (const [k, v] of Object.entries(this.extra())) add(k.replace(/_/g, ' '), fmt(v));
      if (this.memory) add('memory', `${(this.memory.buffer.byteLength / 1048576).toFixed(0)} MB`);
      parts.push(dl);
    }
    this.el.replaceChildren(...parts);
    this.el.dataset.fps = fps.toFixed(1);
    this.el.dataset.source = game ? 'game' : 'page';
  }
}

function fmt(v: number) { return Number.isInteger(v) ? v.toLocaleString('en-US') : v.toFixed(2); }
