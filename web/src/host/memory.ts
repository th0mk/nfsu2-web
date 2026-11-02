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

export interface MemoryTry { initial: number; maximum: number }

/** The sizes to try, best first: initial = the build's pages (game.json "pages") */
export function memoryTries(initial: number): MemoryTry[] {
  return [{ initial, maximum: MAX_PAGES }, { initial, maximum: initial }];
}

/** what a try was, for the log: "1024 MB, up to 4096 MB" */
export function sizeText(t: MemoryTry): string {
  const mb = (p: number) => Math.round(p / 16);
  return t.maximum > t.initial ? `${mb(t.initial)} MB, up to ${mb(t.maximum)} MB` : `${mb(t.initial)} MB, not growing`;
}

/** The first of the tries the browser gives, and a line for the log saying which (and what it refused before it).
 *  Throws the last refusal when it gives none, with the log's line as its note.  make: the constructor (tests pass
 *  their own). */
export function allocMemory(tries: MemoryTry[], make = (t: MemoryTry) => new WebAssembly.Memory({ ...t, shared: true })):
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
      // anything but "too much" (a TypeError: no shared memory at all) is not a size question
      if (!(e instanceof RangeError)) throw e;
      refused.push(`${sizeText(t)} (${e.message})`);
      last = e;
    }
  }
  if (last) (last as { note?: string }).note = `[memory] the browser refused every size: ${refused.join(', then ')}`;
  throw last;
}
