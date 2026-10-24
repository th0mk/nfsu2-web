// Reader for HOST_KIND_STATUS (runtime/web/status_proto.h): a table of named
// counters the runtime publishes in shared memory.  A sequence lock keeps a
// copy consistent: the copy counts only if seq was even and unchanged.

export const HOST_STATUS_MAGIC = 0x31545453;
const MAX = 32, NAME = 16, ITEM = 24, HEAD = 16;

export class StatusReader {
  private td = new TextDecoder();
  constructor(private memory: WebAssembly.Memory, readonly ptr: number) {}

  /** The counters, or null if the block is not (yet) valid or was being written. */
  read(): Map<string, number> | null {
    const buf = this.memory.buffer;
    if (this.ptr + HEAD + MAX * ITEM > buf.byteLength) return null;
    const i32 = new Int32Array(buf, this.ptr, 4);
    if ((Atomics.load(i32, 0) >>> 0) !== HOST_STATUS_MAGIC) return null;
    for (let tries = 0; tries < 4; tries++) {
      const s0 = Atomics.load(i32, 1);
      if (s0 & 1) continue;
      const n = Math.min(Atomics.load(i32, 2), MAX);
      const bytes = new Uint8Array(buf, this.ptr + HEAD, n * ITEM).slice();
      if (Atomics.load(i32, 1) !== s0) continue;
      const dv = new DataView(bytes.buffer);
      const out = new Map<string, number>();
      for (let k = 0; k < n; k++) {
        const nb = bytes.subarray(k * ITEM, k * ITEM + NAME);
        const z = nb.indexOf(0);
        out.set(this.td.decode(nb.subarray(0, z < 0 ? NAME : z)), dv.getFloat64(k * ITEM + NAME, true));
      }
      return out;
    }
    return null;
  }
}
