import type { GameSession } from './game';
import type { TrapState } from '../net/speedtrap';
import { t } from './i18n';
import { pageDialog } from './padnav';

const el = (tag: string, cls = '', text = '') => {
  const node = document.createElement(tag); node.className = cls; node.textContent = text; return node;
};
const speed = (value: number, metric: boolean) => value ? `${Math.round(value / (metric ? 10 : 16.09344))} ${metric ? 'km/h' : 'mph'}` : '—';
/** Only the crossing feedback; navigation belongs to the game maps. */
export class SpeedTrapPanel {
  private result = el('div','speedtrap-result');
  private flash = el('div','speedtrap-flash');
  private session: GameSession | null = null;
  private state: TrapState | null = null;
  private received = 0;
  private resultUntil = 0;
  private timer: ReturnType<typeof setInterval> | null = null;
  private animations: Animation[] = [];
  constructor(private stage: HTMLElement) {
    this.result.dataset.nfsu2Ui = '';
    this.result.hidden = true;
    this.result.setAttribute('role','status'); this.result.setAttribute('aria-live','polite');
    this.flash.setAttribute('aria-hidden','true');
    stage.append(this.flash,this.result);
  }
  start(session: GameSession) {
    this.stop(); this.session = session;
    this.timer = setInterval(() => this.update(), 100);
  }
  stop() {
    if (this.timer !== null) clearInterval(this.timer);
    this.timer = null; this.session = null; this.state = null;
    this.result.hidden = true; this.resultUntil = 0;
    this.cancelAnimations();
  }
  private driving() {
    return (this.session?.status?.read()?.get('drive') ?? 0) % 4 === 2 &&
      !document.hidden && !pageDialog() && !this.stage.classList.contains('photo');
  }
  receive(s: TrapState) {
    const old = this.state;
    if (old?.session === s.session && s.sequence < old.sequence) return;
    // A new session, nearest-camera change, or reconnect baseline must not replay an old shutter.
    const fresh = performance.now() - this.received < 3000;
    this.state = s; this.received = performance.now();
    if (fresh && old?.session === s.session && s.sequence > old.sequence && s.event && s.active && this.driving()) {
      this.capture(s.event, s.rows);
    }
    this.update();
  }
  private cancelAnimations() {
    for (const animation of this.animations) animation.cancel();
    this.animations = [];
  }
  private capture(event: NonNullable<TrapState['event']>, rows: TrapState['rows']) {
    this.cancelAnimations();
    const metric = this.session?.status?.read()?.get('speed_kmh') !== 0;
    this.result.replaceChildren(el('div','speedtrap-result-label',t('Speed trap')),el('strong','',speed(event.speed, metric)));
    const board = el('ol','speedtrap-leaders');
    for (const player of rows.slice(0,3)) {
      const row = el('li',player.me ? 'me' : '');
      row.append(el('span','',player.name),el('b','',speed(player.speed, metric))); board.append(row);
    }
    this.result.append(board);
    this.result.dataset.trap = event.id;
    this.resultUntil = performance.now() + 5000; this.result.hidden = false;
    if (!matchMedia('(prefers-reduced-motion: reduce)').matches) {
      this.animations.push(this.flash.animate([{opacity:0.18},{opacity:0}],{duration:220,easing:'ease-out'}));
      this.animations.push(this.result.animate([{opacity:0,transform:'translate(-50%, -8px)'},
        {opacity:1,transform:'translate(-50%, 0)'}],{duration:180,easing:'ease-out'}));
    }
    this.session?.audio?.shutter?.();
  }
  private update() {
    const s = this.state, visible = !!s?.active && this.driving() && performance.now()-this.received <= 3000;
    if (!visible) { this.resultUntil = 0; this.cancelAnimations(); }
    this.result.hidden = !visible || performance.now() >= this.resultUntil;
  }
}
