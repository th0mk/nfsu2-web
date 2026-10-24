// The one place that knows the layout of the radio's words after the status table (HostStatusRadio in
// runtime/web/radio_proto.h, same order): the page writes the switch, the tags and the sound, the runtime (runtime/radio.c)
// reads them.  RadioLink.open() gives a link only to a game whose block has the radio's magic and version.
const WORDS = ['radio_magic', 'radio_version', 'on', 'tag_seq', 'places', 'req_seq', 'req_index', 'ready_seq', 'failed_seq', 'total', 'written', 'ring', 'ring_frames', 'read'] as const;
type Word = typeof WORDS[number];
export const RADIO_WORDS: readonly string[] = WORDS;
/** What the runtime writes into the first two words at its start (RADIO_MAGIC, RADIO_VERSION in radio_proto.h). */
export const RADIO_MAGIC = 0x31444152, RADIO_VERSION = 1;
const W = Object.fromEntries(WORDS.map((n, i) => [n, i])) as Record<Word, number>;
const CTL = 16 + 32 * 24,   // the status table before them (status.ts: HEAD + MAX * ITEM)
  PLACES = 2, TAG = 44, TEXT = WORDS.length * 4;

export interface Lines { artist: string; title: string; album: string }
/** 0 menus, 1 driving */
export type Place = 0 | 1;

const ascii = (s: string, to: Uint8Array) => {
  to.fill(0);
  [...s].slice(0, TAG - 1).forEach((ch, i) => { const c = ch.codePointAt(0)!; to[i] = c >= 0x20 && c < 0x7f ? c : 63; });
};

export class RadioLink {
  constructor(private memory: WebAssembly.Memory, private ptr: number) {}

  /** The link to the radio's words at the status block, or null: the block is too short, or it is another build's (an older game in the cache). */
  static open(memory: WebAssembly.Memory, ptr: number): RadioLink | null {
    const end = ptr + CTL + TEXT + PLACES * 3 * TAG;
    if (!Number.isInteger(ptr) || ptr < 0 || end > memory.buffer.byteLength || (ptr + CTL) % 4) return null;
    const w = new Int32Array(memory.buffer, ptr + CTL, 2);
    return Atomics.load(w, W.radio_magic) === RADIO_MAGIC && Atomics.load(w, W.radio_version) === RADIO_VERSION ? new RadioLink(memory, ptr) : null;
  }
  private words() { return new Int32Array(this.memory.buffer, this.ptr + CTL, WORDS.length); }

  /** False once the runtime switched the radio off (memory that does not look right). */
  isOn(): boolean { return (Atomics.load(this.words(), W.on) & 1) === 1; }

  /** The runtime adds one to the word too: the page swaps only the value it read, and a lost swap is read again by isOn(). */
  setOn(on: boolean) {
    const w = this.words(), now = Atomics.load(w, W.on);
    if (((now & 1) === 1) !== on) Atomics.compareExchange(w, W.on, now, now + 1);
  }

  /** The next track of each place (0 menus, 1 driving) with its tags, null: no track suits the place. */
  setPlaces(next: readonly [Lines | null, Lines | null]) {
    const w = this.words(), bytes = new Uint8Array(this.memory.buffer, this.ptr + CTL + TEXT, PLACES * 3 * TAG), s0 = Atomics.load(w, W.tag_seq) & ~1;
    Atomics.store(w, W.tag_seq, s0 + 1);
    next.forEach((t, p) => [t?.artist, t?.title, t?.album].forEach((line, k) => ascii(line ?? '', bytes.subarray((p * 3 + k) * TAG, (p * 3 + k + 1) * TAG))));
    Atomics.store(w, W.places, next.reduce((mask, t, p) => mask | (t ? 1 << p : 0), 0));
    Atomics.store(w, W.tag_seq, s0 + 2);
  }

  /** The game's latest track start: its number and its place. */
  request(): { seq: number; place: Place } {
    const w = this.words();
    return { seq: Atomics.load(w, W.req_seq) >>> 0, place: (Atomics.load(w, W.req_index) & 1) as Place };
  }

  /** The track the game asked for is decoded: total frames, none written yet.  Nothing is published while the runtime has no rings. */
  serve(seq: number, total: number): boolean {
    const w = this.words();
    if (!Atomics.load(w, W.ring_frames) || !Atomics.load(w, W.ring)) return false;
    Atomics.store(w, W.written, 0); Atomics.store(w, W.total, total); Atomics.store(w, W.ready_seq, seq);
    return true;
  }
  fail(seq: number) { Atomics.store(this.words(), W.failed_seq, seq); }

  /** Ends the track where the game has read it: the runtime sees the end of the stream and the game goes on to its next song. */
  cut() { const w = this.words(), at = Atomics.load(w, W.read); Atomics.store(w, W.written, at); Atomics.store(w, W.total, at); }

  /** Frames the rings take now (the counters wrap at 32 bits, their difference does not). */
  room(): number {
    const w = this.words(), size = Atomics.load(w, W.ring_frames);
    return Math.max(0, Math.min(size, size - ((Atomics.load(w, W.written) - Atomics.load(w, W.read)) | 0)));
  }

  /** Appends frames to the rings (left, right) and tells the game they are there.  False: the rings are not where the page can trust them. */
  push(left: Float32Array, right: Float32Array): boolean {
    const w = this.words(), size = Atomics.load(w, W.ring_frames), at = Atomics.load(w, W.written), base = Atomics.load(w, W.ring) >>> 0;
    const buf = this.memory.buffer;
    if (!size || (size & (size - 1)) || !base || base + 8 * size > buf.byteLength) return false;
    [left, right].forEach((src, ch) => {
      const ring = new Float32Array(buf, base + 4 * size * ch, size), from = at & (size - 1), first = Math.min(src.length, size - from);
      ring.set(src.subarray(0, first), from);
      ring.set(src.subarray(first), 0);
    });
    Atomics.store(w, W.written, at + left.length);
    return true;
  }
}
