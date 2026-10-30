// The radio: the game's own EA Trax plays the music files of a folder on this device.  The game asks for a track when it starts a song
// (runtime/radio.c, through RadioLink); this decodes the next one of the place the song is for (0 menus, 1 driving) and hands the sound over.
// Each track has a mode (Menu, Race, All, Off) and each place its own queue, which goes on from where it stopped last time, or plays at random
// when the player asks for Shuffle.  Nothing here runs until the radio is used.
import { STORAGE } from '@game/storage';
import type { Lines, Place, RadioLink } from './app/radio-link';
import { keptFolder, keepFolder, reopenFolder, sortedTracks, subfolderOf, type Folder, type Picked } from './radio-folder';
import { plaqueTags, tagsOf } from './radio-tags';

export type Mode = 'menu' | 'racing' | 'all' | 'off';
export const MODES: readonly Mode[] = ['menu', 'racing', 'all', 'off'];
export type PickFolder = () => Promise<Picked | null>;
const MODES_KEY = STORAGE.LOCAL.RADIO_MODES, ON_KEY = STORAGE.LOCAL.RADIO_ON;   // {path in the folder: mode} without the All ones; '1': left on
const NEXT_KEY = STORAGE.LOCAL.RADIO_NEXT, SHUFFLE_KEY = STORAGE.LOCAL.RADIO_SHUFFLE;   // [path of the next track of the menus, of driving]; '1': at random
export const radioWasOn = () => { try { return localStorage.getItem(ON_KEY) === '1'; } catch { return false; } };
const keepRadioOn = (on: boolean) => { try { localStorage.setItem(ON_KEY, on ? '1' : '0'); } catch { /* a private window keeps nothing */ } };
const readShuffle = () => { try { return localStorage.getItem(SHUFFLE_KEY) === '1'; } catch { return false; } };
const readNext = (): unknown[] => { try { const kept = JSON.parse(localStorage.getItem(NEXT_KEY) ?? '[]'); return Array.isArray(kept) ? kept : []; } catch { return []; } };
const readModes = (): Map<string, Mode> => {
  try {
    const kept = Object.entries(JSON.parse(localStorage.getItem(MODES_KEY) ?? '{}') ?? {});
    return new Map(kept.filter(([, m]) => m !== 'all' && MODES.includes(m as Mode)) as [string, Mode][]);
  } catch { return new Map(); }
};

const RATE = 44100, CHUNK = 1 << 15, STEP_MS = 50, TAGS_KEPT = 16;   // the game mixer's rate; frames handed over at a time
const MAX_FILE_BYTES = 160 << 20, MAX_FRAMES = RATE * 15 * 60;       // longer is a mix, not a song; a whole file is decoded into memory
type Pair = [number, number];
const skipLine = (i: number, why: unknown) => `skips #${i + 1}: ${why}`;

export class Radio {
  private files: FileSystemFileHandle[] = [];
  private paths: string[] = [];           // files[i] is at paths[i] in the folder ("Album/a.mp3")
  private name = '';
  private handle: Folder | null = null;
  private modes = readModes();
  private timer: ReturnType<typeof setInterval> | null = null;
  private epoch = 0;                      // counts starts and stops: work begun before a stop or restart gives up
  private live = false;                   // the game has been told the radio is on
  private seq = 0;
  private bad = 0;
  private song: { left: Float32Array; right: Float32Array; at: number } | null = null;
  private decoder: OfflineAudioContext | null = null;
  private decoding: Promise<unknown> = Promise.resolve();   // the decode under way: one track is decoded at a time
  private next: Pair = [0, 0];            // per place: where its queue goes on
  private last: Pair = [-1, -1];          // per place: the track chosen last, which Shuffle does not repeat at once
  private shuffled = readShuffle();
  private offered: Pair = [-1, -1];       // per place: the track whose tags the game was shown
  private tags = new Map<number, Promise<Lines>>();
  private playing: { index: number; place: Place } | null = null;
  private restored: Promise<void>;        // settles when the remembered folder is read, or is known to need a click

  constructor(private port: () => RadioLink | null, private log: (line: string) => void = () => {}) { this.restored = this.restore(); }

  get folder() { return this.name; }
  get on() { return this.timer !== null; }
  /** The tracks as paths in the folder ("a.mp3", "Album/a.mp3") in playlist order; the same array until another folder is chosen. */
  get trackPaths() { return this.paths; }
  get shuffle() { return this.shuffled; }
  mode(name: string): Mode { return this.modes.get(name) ?? 'all'; }
  private suits(name: string, place: Place) { const m = this.mode(name); return m === 'all' || m === (place ? 'racing' : 'menu'); }
  private saveModes() { try { localStorage.setItem(MODES_KEY, JSON.stringify(Object.fromEntries(this.modes))); } catch { /* a private window keeps nothing */ } }
  private fail = (e: unknown) => this.stop(`error: ${e}`);

  /** Shuffle on: the next track of a place is any that suits it but the last one; off: the queue goes on from where it stood. */
  setShuffle(on: boolean) {
    if (on === this.shuffled) return;
    this.shuffled = on;
    try { localStorage.setItem(SHUFFLE_KEY, on ? '1' : '0'); } catch { /* a private window keeps nothing */ }
    this.log(`player sets shuffle ${on ? 'on' : 'off'}`);
    if (this.on) void this.offer().catch(this.fail);
  }

  /** A track that no longer suits the place it plays in gives way to the next one that does; the others count from the next song on. */
  setMode(name: string, mode: Mode) {
    const was = this.mode(name); if (was === mode) return;
    if (mode === 'all') this.modes.delete(name); else this.modes.set(name, mode);
    this.saveModes();
    const epoch = this.epoch, p = this.playing, gone = this.on && p && this.paths[p.index] === name && !this.suits(name, p.place);
    this.log(`player sets #${this.paths.indexOf(name) + 1} from ${was} to ${mode}${gone ? `: it no longer suits ${p.place ? 'driving' : 'menus'}, the next track plays` : ''}`);
    if (!this.on) return;
    void this.offer(epoch).then(() => {
      if (gone && this.playing === p && epoch === this.epoch) { this.song = null; this.port()?.cut(); }
    }).catch(this.fail);
  }

  /** After a reload: opens the remembered folder when the browser still allows it, else the player's next click on Radio asks. */
  private async restore() {
    try {
      const saved = await keptFolder();
      if (this.handle || !saved) return;
      this.handle = saved; this.name = saved.name;
      await this.reopen(false);
    } catch (e) { this.log(`folder not restored: ${e}`); }
  }

  /** Reads the remembered folder; ask: the browser may show its question, which needs a click. */
  private async reopen(ask: boolean) {
    const handle = this.handle;
    if (!handle || this.files.length) return;
    const found = await reopenFolder(handle, ask);
    if (found && this.handle === handle) this.setFiles(found);
  }

  private setFiles(p: { files: readonly FileSystemFileHandle[]; paths?: readonly string[] }) {
    ({ files: this.files, paths: this.paths } = sortedTracks(p.files, p.paths));
    const kept = readNext();
    this.next = [0, 1].map(place => Math.max(0, this.paths.indexOf(String(kept[place])))) as Pair; this.last = [-1, -1]; this.tags = new Map();
    if (!this.paths.length) return;
    const gone = [...this.modes.keys()].filter(k => !this.paths.includes(k));
    gone.forEach(k => this.modes.delete(k));
    if (gone.length) this.saveModes();
  }

  setFolder(p: Picked) {
    this.setFiles(p);
    this.name = p.name;
    this.handle = p.handle ?? null;
    if (p.handle) void keepFolder(p.handle);
    this.log(`folder chosen, ${this.files.length} tracks`);
    if (this.on) { this.stop('the folder changed'); void this.start(); }
  }

  async chooseFolder(pick: PickFolder) {
    await this.restored;
    const p = await pick();
    if (p) this.setFolder(p);
  }

  /** The player's click on Radio: on (asking for the folder when there is none), remembered for the next game. */
  async turnOn(pick: PickFolder) {
    await this.restored;
    try { await this.reopen(true); } catch (e) { this.log(`folder not opened: ${e}`); }
    if (!this.files.length) await this.chooseFolder(pick);
    await this.start();
    keepRadioOn(this.on);
  }

  turnOff() { this.stop(); keepRadioOn(false); }

  /** A game starts and the radio was left on. */
  async resume() { await this.restored; await this.start(); }

  private keepNext() {
    const n = this.paths.length;
    try { localStorage.setItem(NEXT_KEY, JSON.stringify(this.next.map(i => this.paths[i % n]))); } catch { /* a private window keeps nothing */ }
  }

  /** The next track of the place from where its queue stands, the first whose mode suits it; with Shuffle any that suits it but the last (-1: none). */
  private choose(place: Place): number {
    const n = this.files.length;
    if (this.shuffled) {
      const fit = [...this.paths.keys()].filter(i => this.suits(this.paths[i], place)), other = fit.filter(i => i !== this.last[place]);
      const from = other.length ? other : fit;
      return from.length ? from[Math.floor(Math.random() * from.length)] : -1;
    }
    for (let k = 0; k < n; k++) {
      const i = (this.next[place] + k) % n;
      if (this.suits(this.paths[i], place)) return i;
    }
    return -1;
  }

  /** Shows the game the tags of the next track of each place (the same track is read once). */
  private async offer(epoch = this.epoch) {
    const link = this.port(), files = this.files, picks: Pair = [this.choose(0), this.choose(1)];
    if (!link) return;
    const lines = await Promise.all(picks.map(i => i < 0 ? null : this.tagsFor(files, i)));
    if (epoch !== this.epoch || files !== this.files) return;
    this.offered = picks;
    link.setPlaces([lines[0], lines[1]]);
  }

  private tagsFor(files: FileSystemFileHandle[], i: number): Promise<Lines> {
    let t = this.tags.get(i);
    if (!t) {
      if (this.tags.size >= TAGS_KEPT) this.tags.clear();
      t = tagsOf(files[i]).then(l => plaqueTags(l, subfolderOf(this.paths[i]) || this.name));
      this.tags.set(i, t);
    }
    return t;
  }

  /** Any failure turns only the radio off. */
  async start() {
    const link = this.port();
    if (this.on || !link || !this.files.length) return;
    const epoch = ++this.epoch;
    this.bad = 0;
    this.timer = setInterval(() => void this.step().catch(this.fail), STEP_MS);
    try {
      this.seq = link.request().seq;
      this.log(`on, ${this.files.length} tracks`);
      await this.offer(epoch);
      if (epoch === this.epoch) { link.setOn(true); this.live = true; }
    } catch (e) { this.fail(e); }
  }

  stop(why = '') {
    if (this.timer) { clearInterval(this.timer); this.log(why ? `off: ${why}` : 'off'); }
    this.timer = null; this.live = false; this.song = null; this.playing = null; this.epoch++;
    try { this.port()?.setOn(false); } catch (e) { this.log(`the switch was not written: ${e}`); }
  }

  /** The game starts a song: decode the track offered for its place (the next one when a file does not read); then keep the rings full. */
  private async step() {
    const epoch = this.epoch, link = this.port();
    if (!link) return;
    if (this.live && !link.isOn()) return this.stop('the game switched it off');
    const want = link.request();
    if (want.seq === this.seq) { if (this.song) this.feedOrSkip(link); return; }
    const place = want.place, where = place ? 'driving' : 'menus';
    this.seq = want.seq; this.song = null; this.playing = null;
    let i = this.offered[place];
    for (let tried = 0; i >= 0 && tried < this.files.length && this.bad < this.files.length; tried++, i = this.choose(place)) {
      this.next[place] = i + 1; this.last[place] = i; this.keepNext();
      const got = await this.load(link, want.seq, i, epoch);
      if (got === 'stale') return;
      if (got === 'played') {
        this.log(`plays #${i + 1} (${this.mode(this.paths[i])}) in ${where}`);
        this.playing = { index: i, place };
        void this.offer(epoch).catch(this.fail);
        return;
      }
    }
    link.fail(want.seq);
    if (i >= 0) this.stop('no file of the folder plays');
    else { this.log(`silence: no track fits ${where}`); void this.offer(epoch).catch(this.fail); }
  }

  /** A failure of reading, decoding or handing over the sound is the file's ("skipped": the next one plays); the runtime having no rings
   *  at all is the radio's (it turns off), and so is a whole lap of the folder without one track played. */
  private async load(link: RadioLink, seq: number, i: number, epoch: number): Promise<'played' | 'skipped' | 'ring' | 'stale'> {
    let left: Float32Array, right: Float32Array;
    try {
      await this.decoding;                // songs that change quickly never hold two decoded tracks at once
      if (epoch !== this.epoch || this.seq !== seq) return 'stale';
      const file = await this.files[i].getFile();
      if (file.size > MAX_FILE_BYTES) throw new Error(`${Math.round(file.size / 1048576)} MB is too long for a song`);
      const decoder = this.decoder ??= new OfflineAudioContext(2, 1, RATE), decoding = file.arrayBuffer().then(b => decoder.decodeAudioData(b));
      this.decoding = decoding.catch(() => {});
      const pcm = await decoding;
      if (epoch !== this.epoch || this.seq !== seq) return 'stale';
      left = pcm.getChannelData(0); right = pcm.numberOfChannels > 1 ? pcm.getChannelData(1) : left;
      if (!left.length) throw new Error('no samples');
      if (left.length > MAX_FRAMES) throw new Error('longer than 15 minutes');
    } catch (e) { this.log(skipLine(i, e)); return 'skipped'; }
    this.song = { left, right, at: 0 };
    if (!link.serve(seq, left.length)) throw new Error('the game has no rings for the sound');
    try { this.feed(link); } catch (e) { this.song = null; this.log(skipLine(i, e)); this.bad++; return 'ring'; }
    return 'played';
  }

  private feedOrSkip(link: RadioLink) {
    try { this.feed(link); } catch (e) {
      const p = this.playing;
      this.song = null; this.playing = null; this.seq = -1; this.bad++;
      if (p) this.log(skipLine(p.index, e));
    }
  }

  /** Hands the rings what they take; the sound is let go of when its last frame is in. */
  private feed(link: RadioLink) {
    const s = this.song;
    if (!s) return;
    const n = Math.min(link.room(), CHUNK * 4, s.left.length - s.at);
    if (n > 0) {
      if (!link.push(s.left.subarray(s.at, s.at + n), s.right.subarray(s.at, s.at + n))) throw new Error('the rings are not where they should be');
      s.at += n;
    }
    if (s.at >= s.left.length) { this.song = null; this.bad = 0; }
  }
}
