// Shared-engine split-screen input. Each physical pad owns at most one seat.
// START claims a seat; disconnecting preserves its place until a controller
// explicitly claims it. Joining buttons must be released before driving.
import type { PadSnapshot } from './gamepad';
import {
  INPUT_LOCAL_PADS, IM_LOCAL_WORDS, IM_OFF_LOCAL_PADS, IM_LOCAL_SEQ, IM_LOCAL_CONNECTED,
  IM_LOCAL_BUTTONS, IM_LOCAL_FAMILY, IM_LOCAL_AXIS, IM_LOCAL_NAME, IM_LOCAL_INDEX, IM_LOCAL_JOIN, IM_LOCAL_COOP,
} from './proto';

type Owner = { index: number; id: string; waiting: boolean };
export class LocalSeats {
  owners: (Owner | null)[] = Array.from({ length: INPUT_LOCAL_PADS }, () => null);
  private starts = new Map<number, { id: string; down: boolean }>();
  readonly joins = Array(INPUT_LOCAL_PADS).fill(0);

  poll(pads: readonly (Gamepad | null)[]): (Gamepad | null)[] {
    const live = pads.filter((g): g is Gamepad => !!g?.connected && g.mapping === 'standard');
    const present = (o: Owner | null) => o && live.find(g => g.index === o.index && g.id === o.id);
    for (const g of live) {
      const down = !!g.buttons[9]?.pressed || (g.buttons[9]?.value ?? 0) > 0.5;
      const was = this.starts.get(g.index);
      if (down && !(was?.id === g.id && was.down)) {
        let seat = this.owners.findIndex(o => o?.index === g.index && o.id === g.id);
        if (seat < 0) {
          seat = this.owners.findIndex(o => !present(o));
          if (seat >= 0) this.owners[seat] = { index: g.index, id: g.id, waiting: true };
        }
        if (seat >= 0) this.joins[seat] = (this.joins[seat] + 1) >>> 0;
      }
      this.starts.set(g.index, { id: g.id, down });
    }
    for (const index of this.starts.keys()) if (!live.some(g => g.index === index)) this.starts.delete(index);
    return this.owners.map(o => {
      const g = present(o);
      if (!o || !g) { if (o) o.waiting = true; return null; }
      if (o.waiting) {
        if (g.buttons.some(b => b.pressed || b.value > 0.5)) return null;
        o.waiting = false;
      }
      return g;
    });
  }
}

/** One record per seat in the SAME game's shared memory, not six processes. */
export class LocalPadWriter {
  private seats = new LocalSeats();
  private words: Int32Array;
  private bits = new Float32Array(1);
  private ints = new Int32Array(this.bits.buffer);
  constructor(buffer: SharedArrayBuffer | ArrayBuffer, base: number) {
    this.words = new Int32Array(buffer, base + IM_OFF_LOCAL_PADS, INPUT_LOCAL_PADS * IM_LOCAL_WORDS);
  }
  /** the controllers (Gamepad.index) the game keeps for co-op free roam's player 2: they drive nothing else */
  reserved(): Set<number> {
    const out = new Set<number>();
    this.seats.owners.forEach((o, seat) => { if (o && Atomics.load(this.words, seat * IM_LOCAL_WORDS + IM_LOCAL_COOP) === 1) out.add(o.index); });
    return out;
  }
  poll(pads: readonly (Gamepad | null)[], read: (g: Gamepad) => PadSnapshot, muted = false) {
    this.seats.poll(pads).forEach((g, seat) => {
      const state = g && !muted ? read(g) : null;
      const at = seat * IM_LOCAL_WORDS, w = this.words;
      const seq = Atomics.load(w, at + IM_LOCAL_SEQ);
      Atomics.store(w, at + IM_LOCAL_SEQ, (seq + 1) | 0);
      Atomics.store(w, at + IM_LOCAL_CONNECTED, state ? 1 : 0);
      Atomics.store(w, at + IM_LOCAL_BUTTONS, state?.buttons ?? 0);
      Atomics.store(w, at + IM_LOCAL_FAMILY, state?.family ?? 0);
      Atomics.store(w, at + IM_LOCAL_INDEX, g?.index ?? -1);
      Atomics.store(w, at + IM_LOCAL_JOIN, this.seats.joins[seat]);
      for (let i = 0; i < 6; i++) {
        this.bits[0] = state?.axes[i] ?? 0;
        Atomics.store(w, at + IM_LOCAL_AXIS + i, this.ints[0]);
      }
      const name = (state?.name ?? '').slice(0, 63);
      for (let i = 0; i < 16; i++) {
        let value = 0;
        for (let b = 0; b < 4; b++) value |= ((name.charCodeAt(i * 4 + b) || 0) & 255) << (b * 8);
        Atomics.store(w, at + IM_LOCAL_NAME + i, value);
      }
      Atomics.store(w, at + IM_LOCAL_SEQ, (seq + 2) | 0);
    });
  }
}
