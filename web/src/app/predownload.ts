// The game data (all but the movies) onto this PC ahead of play ("Game data on this PC", at
// the bottom of the start page): the predownload worker fills the cache the
// game reads from.  It only runs while no game runs in this tab, because the
// game's I/O worker writes the same cache files.  A game pauses it, and it
// goes on after the game ends.

import type { FromPredownload, ToPredownload } from '../host/predownload-worker';
import { DATA_BASE, MANIFEST_URL, aheadFiles, languageMovies, manifestFiles, scanCache, type Manifest } from '../host/datacache';

export type DownloadState = 'idle' | 'running' | 'paused' | 'done' | 'error';
export interface DownloadStatus {
  state: DownloadState;
  /** bytes of the game data in this browser, of total (0 until the worker has counted them) */
  have: number; total: number;
  /** bytes a second over the last 10 s, 0 until measured */
  rate: number;
  error?: string;
}

const STOP_WAIT_MS = 3000;

/** Whether every movie a game in this language can play is in this browser's cache (they are not downloaded ahead:
 *  only one that played is), in VP6 or, where the browser plays them in AV1 (av1), their AV1 copies. */
export async function moviesKept(language: string, av1: boolean, voices = 'dubbed'): Promise<boolean> {
  const r = await fetch(MANIFEST_URL, { cache: 'no-cache' });
  if (!r.ok) return false;
  const s = await scanCache(languageMovies(manifestFiles(await r.json() as Manifest), language, av1, voices));
  return s.have >= s.total;
}

/** How much of what the download fetches (all but the movies) this browser keeps: bytes in the cache, of their total. */
export async function gameDataKept(language: string, texpack: string, voices = 'dubbed'): Promise<{ have: number; total: number }> {
  const r = await fetch(MANIFEST_URL, { cache: 'no-cache' });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  const s = await scanCache(aheadFiles(manifestFiles(await r.json() as Manifest), language, texpack, voices));
  return { have: s.have, total: s.total };
}

export class Predownload {
  status: DownloadStatus = { state: 'idle', have: 0, total: 0, rate: 0 };
  private w: Worker | null = null;
  private ended: (() => void) | null = null;
  private samples: { t: number; have: number }[] = [];

  /** language: the game's language now (settings.ini): only its language pack is fetched; texpack: the texture
   *  pack picked ('' for none), the only one fetched; voices: 'dubbed' | 'original' */
  constructor(private onChange: (s: DownloadStatus) => void, private log: (line: string) => void, readonly language: () => string,
              readonly texpack: () => string, readonly voices: () => string = () => 'dubbed') {}

  get running() { return this.w !== null; }

  start() {
    if (this.w) return;
    const w = this.w = new Worker(new URL('/host/predownload-worker.js', import.meta.url), { type: 'module', name: 'predownload' });
    this.samples = [];
    w.onmessage = (e: MessageEvent<FromPredownload>) => this.message(w, e.data);
    w.onerror = e => this.message(w, { t: 'error', msg: e.message || 'it stopped unexpectedly' });
    w.postMessage({ t: 'start', manifestUrl: MANIFEST_URL, dataBase: DATA_BASE, language: this.language(), texpack: this.texpack(), voices: this.voices() } satisfies ToPredownload);
    this.set({ state: 'running', have: 0, total: 0, rate: 0, error: undefined });     // counted again by the worker
    this.log('[app] predownload: started');
  }

  /** Stop it, and do not resume it after a game (it keeps what it wrote); resolves once its files are closed. */
  async stop(): Promise<void> {
    if (!this.w && this.status.state !== 'paused') return;
    await this.end();
    this.set({ state: 'idle', rate: 0 });
    this.log('[app] predownload: stopped');
  }

  /** Before a game: stop it if it runs.  It goes on at resume(). */
  async pause(): Promise<void> {
    if (!this.w) return;
    await this.end();
    this.set({ state: 'paused', rate: 0 });
    this.log('[app] predownload: paused for the game');
  }

  /** After the game: on again if pause() stopped it. */
  resume() { if (this.status.state === 'paused') this.start(); }

  private end(): Promise<void> {
    const w = this.w;
    if (!w) return Promise.resolve();
    return new Promise<void>(res => {
      const timer = setTimeout(() => { this.log('[app] predownload: did not stop in time, ended'); finish(); }, STOP_WAIT_MS);
      const finish = () => { clearTimeout(timer); w.terminate(); if (this.w === w) this.w = null; this.ended = null; res(); };
      this.ended = finish;
      w.postMessage({ t: 'stop' } satisfies ToPredownload);
    });
  }

  private message(w: Worker, m: FromPredownload) {
    if (w !== this.w) return;
    switch (m.t) {
      case 'progress': {
        const t = performance.now();
        this.samples.push({ t, have: m.have });
        while (this.samples.length > 2 && t - this.samples[0].t > 10_000) this.samples.shift();
        const a = this.samples[0], span = (t - a.t) / 1000;
        this.set({ have: m.have, total: m.total, rate: span >= 2 ? (m.have - a.have) / span : 0 });
        return;
      }
      case 'stopped': this.ended?.(); return;
      case 'done':
        w.terminate(); this.w = null;
        this.log(`[app] predownload: done, ${m.total} bytes of game data in this browser`);
        this.set({ state: 'done', have: m.have, total: m.total, rate: 0 });
        return;
      case 'error':
        w.terminate(); this.w = null;
        this.log(`[app] predownload: stopped, ${m.msg}`);
        this.set({ state: 'error', rate: 0, error: m.msg });
        this.ended?.();
    }
  }

  private set(p: Partial<DownloadStatus>) { this.status = { ...this.status, ...p }; this.onChange(this.status); }
}
