// Windows' AltGr (issue 58).  With a keyboard layout that has AltGr (German, French, Turkish, Polish, the Nordic ones
// ...) Windows sends the right Alt key as a Left Ctrl press and then Right Alt, and Chrome passes both on.  The game has
// Shift Down on Left Ctrl and Nitrous on Right Alt, so every nitrous on that key also shifted down; the PC game only
// ever got the nitrous.  Once this browser has shown that it sends that Left Ctrl (a Right Alt as AltGraph right after
// a Left Ctrl press), a Left Ctrl press waits up to WAIT_MS for the next key: Right Alt as AltGraph drops it, any other
// key (or the end of the wait) sends it first.  So on such a layout a Left Ctrl alone reaches the game up to WAIT_MS
// later, and on other layouts nothing changes.  The first AltGr in a browser still shifts down once; localStorage keeps
// what it showed.  Releases always go on: the game ignores the release of a key it does not hold.

/** how long a Left Ctrl press waits for AltGr's Right Alt (Windows sends the two together; this is the margin) */
export const WAIT_MS = 25;
/** a Right Alt as AltGraph this soon after a Left Ctrl press (still down) shows that the browser sends AltGr's Left Ctrl */
const LEARN_MS = 50;
const STORE_KEY = 'nfsu2.altgr-ctrl';

export interface AltGrKey { code: string; key: string; repeat: boolean }
type Store = Pick<Storage, 'getItem' | 'setItem'>;

function localStore(): Store | null {
  try { return typeof localStorage === 'undefined' ? null : localStorage; } catch { return null; }
}

export class AltGrCtrl {
  private held: (() => void) | null = null;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private heldAt = 0;
  private ctrlAt = -Infinity;          // the last Left Ctrl press, while it is down
  private dropped = false;             // AltGr's Left Ctrl was dropped: so are its repeats, until its release
  private seen = false;
  private said = false;

  /** on: Windows (no other system sends AltGr as two keys) */
  constructor(private readonly on: boolean, private readonly log: (s: string) => void,
              private readonly now: () => number = () => performance.now(), private readonly store: Store | null = localStore()) {
    if (on) try { this.seen = store?.getItem(STORE_KEY) === '1'; } catch { /* no storage: learnt again in this tab */ }
  }

  /** a key's event on its way to the game: send goes now, a little later, or (AltGr's Left Ctrl) never */
  key(e: AltGrKey, down: boolean, send: () => void): void {
    if (!this.on) { send(); return; }
    const altGr = down && e.code === 'AltRight' && e.key === 'AltGraph';
    if (altGr && this.held) {
      const gap = this.now() - this.heldAt;
      this.cancel();
      this.dropped = true;
      if (!this.said) { this.said = true; this.log(`[keys] AltGr: its Left Ctrl (${gap.toFixed(1)} ms before Right Alt) does not reach the game`); }
      send();
      return;
    }
    this.flush();
    if (altGr && !this.seen && this.now() - this.ctrlAt < LEARN_MS) {
      this.seen = true;
      try { this.store?.setItem(STORE_KEY, '1'); } catch { /* kept for this tab only */ }
      this.log('[keys] this browser sends AltGr as Left Ctrl and Right Alt: from now on a Left Ctrl right before Right Alt does not reach the game');
    }
    if (e.code === 'ControlLeft') {
      if (!down) { this.ctrlAt = -Infinity; this.dropped = false; }
      else if (e.repeat) { if (this.dropped) return; }
      else {
        this.ctrlAt = this.now();
        this.dropped = false;
        if (this.seen) {
          this.held = send; this.heldAt = this.ctrlAt;
          this.timer = setTimeout(() => this.flush(), WAIT_MS);
          return;
        }
      }
    }
    send();
  }

  /** a Left Ctrl held back goes to the game now */
  flush(): void {
    const f = this.held;
    this.cancel();
    f?.();
  }

  /** a Left Ctrl held back goes nowhere (the input layer is going) */
  cancel(): void {
    if (this.timer !== null) clearTimeout(this.timer);
    this.timer = null;
    this.held = null;
  }
}
