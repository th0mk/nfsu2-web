// A player who leaves the game running gives their place on the server back.  After inputMs without a key,
// the mouse or a controller (input/index.ts lastInput), a warning counts down warnMs; then the page lets the
// server's switch go (net/index.ts setAway): Online and LAN play lose their connection, free roam its friends,
// and the place is free for another player.  A tab in the background gets no warning anyone would see: after
// hiddenMs there the place goes at once.  The next input takes the link up again (a new address; the game's
// Online mode logs in again).  Only while the game holds a place (free roam, the LAN and Online screens), and
// never while the page cannot see the input (the game's own driver plays an input script).

import type { InputSubsystem, NetSubsystem } from './contract';
import { t } from './i18n';

export interface AwayLimits { inputMs: number; hiddenMs: number; warnMs: number }
export const AWAY_LIMITS: AwayLimits = { inputMs: 12 * 60_000, hiddenMs: 8 * 60_000, warnMs: 2 * 60_000 };

const TICK_MS = 1000;
const span = (ms: number) => ms >= 60_000 ? `${Math.round(ms / 60_000)} min` : `${Math.round(ms / 1000)} s`;

export class AwayCheck {
  private timer: ReturnType<typeof setInterval>;
  private hiddenSince: number | null = document.hidden ? performance.now() : null;
  /** when the warning came up, and when the place went back */
  private warnedAt: number | null = null;
  private awayAt: number | null = null;

  /** s: the running game's subsystems (they attach later); show(text): the message over the game, null: none */
  constructor(private s: { input: InputSubsystem | null; net: NetSubsystem | null }, private limits: AwayLimits,
              private show: (text: string | null) => void, private log: (line: string) => void) {
    document.addEventListener('visibilitychange', this.onVisibility);
    this.timer = setInterval(() => this.tick(), TICK_MS);
  }

  private onVisibility = () => {
    this.hiddenSince = document.hidden ? this.hiddenSince ?? performance.now() : null;
    this.tick();
  };

  private tick() {
    const net = this.s.net, last = this.s.input?.lastInput?.() ?? null, now = performance.now(), L = this.limits;
    if (!net?.away || last === null) return;
    if (this.awayAt !== null) {
      if (last <= this.awayAt) return;
      this.awayAt = null; this.show(null);
      if (this.hiddenSince !== null) this.hiddenSince = now;      // a controller's button in a background tab
      net.away(false);
      this.log('[app] away: the player is back, the game takes a place on the server again when it needs one');
      return;
    }
    if (this.warnedAt !== null && last > this.warnedAt) {
      this.warnedAt = null; this.show(null);
      this.log('[app] away: input again, the place on the server stays');
    }
    if (!net.holding?.()) { if (this.warnedAt !== null) { this.warnedAt = null; this.show(null); } return; }
    const hidden = this.hiddenSince !== null && now - this.hiddenSince >= L.hiddenMs;
    if (hidden || (this.warnedAt !== null && now - this.warnedAt >= L.warnMs)) {
      this.warnedAt = null; this.awayAt = now;
      net.away(true);
      this.log(`[app] away: ${hidden ? `the tab was in the background for ${span(now - this.hiddenSince!)}` : `no input for ${span(now - last)}`}, ` +
               'so the place on the server goes back');
      this.show(t('You were away, so you left multiplayer and freed your place on the server. Press any key or button to rejoin.'));
      return;
    }
    if (this.warnedAt === null && now - last >= L.inputMs) {
      this.warnedAt = now;
      this.log(`[app] away: no input for ${span(now - last)}: the place on the server goes back in ${span(L.warnMs)} unless there is some`);
    }
    if (this.warnedAt !== null)
      this.show(t('Still there? You leave multiplayer in {n} s. Press any key or button to stay.',
        { n: Math.max(1, Math.ceil((L.warnMs - (now - this.warnedAt)) / 1000)) }));
  }

  stop() {
    clearInterval(this.timer);
    document.removeEventListener('visibilitychange', this.onVisibility);
    this.show(null);
  }
}
