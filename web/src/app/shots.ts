// The screenshots part of the Saves section.  F12 while the game runs saves the
// next frame as a PNG at the top of the save folder (shell.ts screenshot,
// saveScreenshot here), so a backup carries it with the saves.  Shown newest
// first, four to a row, the newest eight.  One opens in the #shot-view dialog
// to page through all of them (the arrow keys too), download or delete.
// Thumbnails are made one at a time (a 4K PNG takes about 0.1 s to decode) and
// kept while the page lives.  TGA files (what the game's own F12 wrote before
// the page took the key, black in this port, and the PC game's) are decoded
// here: browsers show no TGA.

import { exists, removePath, saveRoot, writeFile, type Entry } from './savefs';
import { downloadBlob, stamp } from './logs';
import { FILE_PREFIX } from '@game/run';
import { fmtBytes } from './checks';
import { dateLocale, t } from './i18n';

export interface ShotsDeps {
  saveDir: string;
  confirm(title: string, body: Node | string, ok: string, danger?: boolean): Promise<boolean>;
  /** a screenshot was deleted: the section lists the save folder again */
  changed(): void;
}

/** two rows of four, the newest; the dialog pages through the rest */
const SHOWN = 8;
const THUMB_W = 384;
const MIME: Record<string, string> = { png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg', bmp: 'image/bmp', tga: 'image/x-tga' };
const ext = (name: string) => name.slice(name.lastIndexOf('.') + 1).toLowerCase();
const key = (s: Entry) => `${s.path}|${s.size}|${s.mtime}`;

/** Write an F12 screenshot at the top of the save folder as nfsu2-<date>_<time>.png (nfsmw- in Most Wanted: run.ts
 *  FILE_PREFIX); returns its name. */
export async function saveScreenshot(saveDir: string, png: Blob, d = new Date()): Promise<string> {
  const base = `${FILE_PREFIX}-${stamp(d)}`;
  let name = `${base}.png`;
  for (let n = 2; await exists(saveDir, name); n++) name = `${base}-${n}.png`;
  await writeFile(saveDir, name, png);
  return name;
}

/**
 * A TGA's pixels: true colour, uncompressed or RLE (types 2 and 10), 24 or 32 bits, either row order.  Alpha is
 * left out (the game's are X8R8G8B8, alpha 0).  null: another kind, or cut short.
 */
export function decodeTga(b: Uint8Array): ImageData | null {
  if (b.length < 18) return null;
  const type = b[2], w = b[12] | b[13] << 8, h = b[14] | b[15] << 8, bpp = b[16], top = (b[17] & 0x20) !== 0;
  if (b[1] !== 0 || (type !== 2 && type !== 10) || (bpp !== 24 && bpp !== 32) || !w || !h) return null;
  const px = bpp >> 3, out = new ImageData(w, h), d = out.data;
  let p = 18 + b[0], x = 0, y = 0, o = top ? 0 : (h - 1) * w * 4;
  const put = (s: number) => {
    d[o] = b[s + 2]; d[o + 1] = b[s + 1]; d[o + 2] = b[s]; d[o + 3] = 255; o += 4;
    if (++x === w) { x = 0; y++; o = (top ? y : h - 1 - y) * w * 4; }
  };
  if (type === 2) {
    if (p + w * h * px > b.length) return null;
    for (; y < h; p += px) put(p);
    return out;
  }
  while (y < h) {
    if (p >= b.length) return null;
    const c = b[p++], n = (c & 127) + 1;
    if (p + (c & 128 ? 1 : n) * px > b.length) return null;
    if (c & 128) { for (let i = 0; i < n && y < h; i++) put(p); p += px; }
    else for (let i = 0; i < n && y < h; i++, p += px) put(p);
  }
  return out;
}

/** "Today, 14:03", "Yesterday, 22:17", "20 Oct, 21:44" (with the year when it is not this one) */
export function when(at: number, now = new Date()): string {
  const d = new Date(at), time = d.toLocaleTimeString(dateLocale(), { hour: '2-digit', minute: '2-digit' });
  const day = (x: Date) => Date.UTC(x.getFullYear(), x.getMonth(), x.getDate());
  const ago = Math.round((day(now) - day(d)) / 864e5);
  if (ago === 0) return t('Today, {time}', { time });
  if (ago === 1) return t('Yesterday, {time}', { time });
  const year = d.getFullYear() !== now.getFullYear() ? { year: 'numeric' as const } : {};
  return `${d.toLocaleDateString(dateLocale(), { day: 'numeric', month: 'short', ...year })}, ${time}`;
}

async function fileOf(saveDir: string, name: string): Promise<File> {
  return (await (await saveRoot(saveDir, false)).getFileHandle(name)).getFile();
}

/** a screenshot decoded (resizeWidth: made that wide on the way) */
async function bitmap(saveDir: string, name: string, resizeWidth?: number): Promise<ImageBitmap> {
  const f = await fileOf(saveDir, name);
  const o: ImageBitmapOptions = resizeWidth ? { resizeWidth, resizeQuality: 'medium' } : {};
  if (ext(name) !== 'tga') return createImageBitmap(f, o);
  const img = decodeTga(new Uint8Array(await f.arrayBuffer()));
  if (!img) throw new Error('not a TGA this page reads');
  return createImageBitmap(img, o);
}

async function encode(bmp: ImageBitmap, type: string): Promise<Blob> {
  const c = new OffscreenCanvas(bmp.width, bmp.height);
  c.getContext('2d')!.drawImage(bmp, 0, 0);
  bmp.close();
  return c.convertToBlob({ type, quality: 0.85 });
}

export class ScreenshotsPart {
  private shots: Entry[] = [];
  /** thumbnails: key -> object URL of a small JPEG, null when the file cannot be drawn */
  private thumbs = new Map<string, Promise<string | null>>();
  private queue: Promise<unknown> = Promise.resolve();
  private dlg: HTMLDialogElement;
  /** the one in the dialog, its object URL; seq: the latest open() wins */
  private open: string | null = null;
  private url: string | null = null;
  private seq = 0;

  constructor(private root: HTMLElement, private deps: ShotsDeps) {
    this.dlg = root.ownerDocument.querySelector<HTMLDialogElement>('#shot-view')!;
    this.dlg.addEventListener('click', e => {
      const a = (e.target as HTMLElement).closest<HTMLButtonElement>('[data-act]')?.dataset.act;
      if (a === 'newer') this.step(-1); else if (a === 'older') this.step(1);
      else if (a === 'download') void this.download(); else if (a === 'delete') void this.remove();
    });
    this.dlg.addEventListener('keydown', e => {
      if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
      e.preventDefault();
      this.step(e.key === 'ArrowLeft' ? -1 : 1);
    });
    this.dlg.addEventListener('close', () => {
      this.open = null; this.seq++;
      this.$<HTMLImageElement>('#shot-img').removeAttribute('src');
      if (this.url) { URL.revokeObjectURL(this.url); this.url = null; }
    });
  }

  private $<T extends HTMLElement>(sel: string): T { return this.dlg.querySelector<T>(sel)!; }

  /** The screenshots as the save folder has them now (saves-ui.ts refresh). */
  show(files: Entry[]) {
    this.shots = [...files].sort((a, b) => b.mtime - a.mtime || (a.path < b.path ? 1 : -1));
    const keep = new Set(this.shots.map(key));
    for (const [k, p] of this.thumbs) if (!keep.has(k)) { this.thumbs.delete(k); void p.then(u => u && URL.revokeObjectURL(u)); }
    this.render();
    if (!this.open) return;
    if (this.at() < 0) this.dlg.close(); else this.showPlace();
  }

  private at() { return this.shots.findIndex(s => s.path === this.open); }

  private render() {
    const n = this.shots.length;
    this.root.querySelector<HTMLElement>('#shots-info')!.hidden = n > 0;
    const ul = this.root.querySelector<HTMLUListElement>('#shots')!;
    ul.replaceChildren(...this.shots.slice(0, SHOWN).map((s, i) => {
      const b = this.tile(s);
      b.setAttribute('aria-label', t('Screenshot, {when}', { when: when(s.mtime) }));
      b.title = when(s.mtime);
      b.onclick = () => void this.view(i);
      const li = document.createElement('li'); li.append(b);
      return li;
    }));
  }

  /** a tile with the screenshot's thumbnail, once it is made (its kind in words if it cannot be drawn) */
  private tile(s: Entry): HTMLButtonElement {
    const b = document.createElement('button');
    b.type = 'button'; b.className = 'shot';
    void this.thumb(s).then(u => {
      if (u) b.prepend(Object.assign(document.createElement('img'), { src: u, alt: '', decoding: 'async' }));
      else b.prepend(Object.assign(document.createElement('span'), { className: 'kind', textContent: ext(s.path).toUpperCase() }));
    });
    return b;
  }

  private thumb(s: Entry): Promise<string | null> {
    let p = this.thumbs.get(key(s));
    if (!p) {
      p = this.queue.then(() => bitmap(this.deps.saveDir, s.path, THUMB_W))
        .then(bmp => encode(bmp, 'image/jpeg')).then(b => URL.createObjectURL(b), () => null);
      this.queue = p;
      this.thumbs.set(key(s), p);
    }
    return p;
  }

  /** open screenshot i in the dialog (or show it there in place of the one open) */
  private async view(i: number) {
    const s = this.shots[i], seq = ++this.seq, img = this.$<HTMLImageElement>('#shot-img');
    this.open = s.path;
    this.$('#shot-when').textContent = when(s.mtime);
    this.showPlace();
    if (!this.dlg.open) this.dlg.showModal();
    img.removeAttribute('src');
    let url: string;
    try {
      url = URL.createObjectURL(ext(s.path) === 'tga' ? await encode(await bitmap(this.deps.saveDir, s.path), 'image/png')
        : await fileOf(this.deps.saveDir, s.path));
    } catch (e) {
      if (seq === this.seq) this.$('#shot-meta').textContent = t('{place}. It cannot be shown here ({error}): Download keeps it as it is.', { place: this.place(), error: e instanceof Error ? e.message : String(e) });
      return;
    }
    if (seq !== this.seq) { URL.revokeObjectURL(url); return; }
    if (this.url) URL.revokeObjectURL(this.url);
    this.url = url;
    img.alt = t('Screenshot, {when}', { when: when(s.mtime) });
    img.src = url;
    try { await img.decode(); } catch { /* shown as the browser can */ }
    if (seq === this.seq && img.naturalWidth) this.$('#shot-meta').textContent = `${this.place()} · ${img.naturalWidth} × ${img.naturalHeight} · ${fmtBytes(s.size)}`;
  }

  private place() { return t('{n} of {total}', { n: this.at() + 1, total: this.shots.length }); }

  /** the open one's place in the list, and which way the dialog can go from it */
  private showPlace() {
    const i = this.at(), s = this.shots[i];
    this.$('#shot-meta').textContent = `${this.place()} · ${fmtBytes(s.size)}`;
    this.$<HTMLButtonElement>('[data-act="newer"]').disabled = i <= 0;
    this.$<HTMLButtonElement>('[data-act="older"]').disabled = i >= this.shots.length - 1;
  }

  private step(d: number) {
    const i = this.at() + d;
    if (this.open && i >= 0 && i < this.shots.length) void this.view(i);
  }

  private async download() {
    const s = this.shots[this.at()];
    if (s) downloadBlob(await fileOf(this.deps.saveDir, s.path), s.path, MIME[ext(s.path)]);
  }

  private async remove() {
    const i = this.at(), s = this.shots[i];
    if (!s || !await this.deps.confirm(t('Delete this screenshot?'), t('{file} goes from this browser. A backup you downloaded keeps its copy.', { file: s.path }), t('Delete'), true)) return;
    try { await removePath(this.deps.saveDir, s.path); }
    catch (e) { this.$('#shot-meta').textContent = t('It was not deleted: {error}', { error: String(e) }); return; }
    this.shots.splice(i, 1);
    this.render();
    if (!this.shots.length) this.dlg.close();
    else void this.view(Math.min(i, this.shots.length - 1));
    this.deps.changed();
  }
}
