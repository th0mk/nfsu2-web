// The game's shared memory: the size the build asks for, able to grow to 4 GB, or the same size that can't grow
// where the browser refuses that.
//
// Phones with 2-4 GB failed at Play with "WebAssembly.Memory(): could not allocate memory" (Chrome) or "Out of
// memory" (an iPhone's Home Screen app).  From the engines' sources:
//   - Chrome for Android is a 32-bit program on most such phones (64-bit went to 8 GB phones).  32-bit V8 sets aside
//     min(maximum, 1 GB) of address space for a shared memory, at least its initial size, in one piece, until the
//     memory is collected (v8 src/wasm/wasm-objects.cc WasmMemoryObject::New, src/objects/backing-store.cc).  Its
//     4 GB of addresses come in pieces: 32-bit Chrome 156 refused a page's second gigabyte while the first game's
//     memory was still held (touch.ts kept it), with the players' "could not allocate memory".
//   - Safari keeps every buffer in one 16 GB area (bmalloc Gigacage.h).  A shared memory takes 4 GB of it plus a
//     guard, or else its maximum, in one piece (JavaScriptCore runtime/BufferMemoryHandle.cpp, wasm/WasmMemory.cpp);
//     after a few such memories that area has no 4 GB piece left (WebKit bug 269937).  The same size with a maximum
//     of 1 GB needs only a 1 GB piece.
// Nothing smaller than the build's size: with its graphics the game uses ~930 MB of it from its first menus (brk= in
// its [status] lines), and a 32-bit V8 that can't set aside a memory's maximum quietly sets aside less, down to its
// initial size, so a smaller start that was to grow could not.

const MAX_PAGES = 65536;                    // 4 GB
// The guest heap's window above the runtime, up to 2 GB (runtime/guest_span.h GWIN_HI): asked for first on a computer
// (app/device.ts heapWindow, NFSU2_HEAP_WINDOW=1), and the game uses it only if the memory is that large at its start.
// Elsewhere, or where the browser refuses it, the tries of before, unchanged.
export const WINDOW_PAGES = 32768;

/** optional: the guest heap's window's try, which any refusal passes over (Firefox reports a memory it could not
 *  commit as "out of memory", not a RangeError); a TypeError (no shared memory at all) still stops */
export interface MemoryTry { initial: number; maximum: number; optional?: boolean }

/** The sizes to try, best first: initial = the build's pages (game.json "pages"); window: 2 GB first (the guest
 *  heap's window) */
export function memoryTries(initial: number, window = false): MemoryTry[] {
  const plain = [{ initial, maximum: MAX_PAGES }, { initial, maximum: initial }];
  return window && initial < WINDOW_PAGES ? [{ initial: WINDOW_PAGES, maximum: MAX_PAGES, optional: true }, ...plain] : plain;
}

/** what a try was, for the log: "1024 MB, up to 4096 MB" */
export function sizeText(t: MemoryTry): string {
  const mb = (p: number) => Math.round(p / 16);
  return t.maximum > t.initial ? `${mb(t.initial)} MB, up to ${mb(t.maximum)} MB` : `${mb(t.initial)} MB, not growing`;
}

/** The first of the tries the browser gives, and a line for the log saying which (and what it refused before it).
 *  Throws the last refusal when it gives none, with the log's line as its note.  make: the constructor (tests pass
 *  their own). */
export function allocMemory(tries: MemoryTry[], make = (t: MemoryTry) => new WebAssembly.Memory({ initial: t.initial, maximum: t.maximum, shared: true })):
    { memory: WebAssembly.Memory; note: string } {
  const refused: string[] = [];
  let last: unknown = null;
  for (const t of tries) {
    try {
      const memory = make(t);
      const note = refused.length
        ? `[memory] ${sizeText(t)}: the browser refused ${refused.join(', then ')}`
        : `[memory] ${sizeText(t)}`;
      return { memory, note };
    } catch (e) {
      // anything but "too much" (a TypeError: no shared memory at all) is not a size question; the window's try
      // passes over any other refusal
      if (!(e instanceof RangeError) && !(t.optional && !(e instanceof TypeError))) throw e;
      refused.push(`${sizeText(t)} (${e instanceof Error ? e.message : String(e)})`);
      last = e;
    }
  }
  if (last) (last as { note?: string }).note = `[memory] the browser refused every size: ${refused.join(', then ')}`;
  throw last;
}

// In WebKit the page holds the memory's buffer, not the WebAssembly.Memory.  JavaScriptCore lets a page or worker
// that holds a live Memory of the game's size take about as much again in garbage before it collects: in a two-minute
// crowded free roam the page collected nothing from the game's start to its end and had 650 MB of garbage by then
// (Playwright's WebKit as an iPhone, 2026-11-07, JSC_logGC), ~5 MB a second of its frames' and the network's objects.
// With the buffer alone it collected every ~15-40 MB.  A Memory made on the page sets off a collection as it is made,
// which counts it however soon the page lets it go (one game in three collected nothing all the same), so the I/O
// worker makes it, sends the page its buffer and hands the Memory to each game thread (their module's instance needs
// it) on a port the page gives both; it tells the page each new buffer as the memory grows (io-worker.ts memGrew),
// which the page passes to the network's worker.  (A worker collects as little with the buffer as with the Memory.)

/** the memory as the page and the network's worker use it: its buffer (a WebAssembly.Memory is one too) */
export interface GameMemory { readonly buffer: ArrayBufferLike }

/** whether the page holds the memory's buffer only: in WebKit (Safari, and every browser on an iPhone or iPad) */
export function memoryOffPage(ua = typeof navigator === 'undefined' ? '' : navigator.userAgent): boolean {
  return /AppleWebKit\//.test(ua) && !/Chrome\/|Chromium\/|Firefox\//.test(ua);
}

/** The memory's buffer, the newest one as the memory grows (a shared memory's buffer keeps the length it had). */
export class MemoryBuffer implements GameMemory {
  private waiting: { bytes: number; done: (ok: boolean) => void }[] = [];
  constructor(public buffer: SharedArrayBuffer) {}
  /** a buffer of the grown memory: taken when it is longer than this one */
  grew(b: SharedArrayBuffer): boolean {
    if (!(b instanceof SharedArrayBuffer) || b.byteLength <= this.buffer.byteLength) return false;
    this.buffer = b;
    this.waiting = this.waiting.filter(w => { if (w.bytes > b.byteLength) return true; w.done(true); return false; });
    return true;
  }
  /** resolves true once the buffer has `bytes` bytes, false after ms without */
  until(bytes: number, ms = 10_000): Promise<boolean> {
    if (bytes <= this.buffer.byteLength) return Promise.resolve(true);
    return new Promise(res => {
      const w = { bytes, done: (ok: boolean) => { clearTimeout(t); res(ok); } };
      const t = setTimeout(() => { this.waiting = this.waiting.filter(x => x !== w); res(false); }, ms);
      this.waiting.push(w);
    });
  }
}
