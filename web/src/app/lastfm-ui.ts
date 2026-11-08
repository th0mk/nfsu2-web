// The Game tab's Last.fm row, under the songs' settings: Connect (Last.fm's "allow access" page in a popup, lastfm.ts), then who
// scrobbles and Disconnect; under it one line about .fmbot, the Discord bot that shows Last.fm stats.  Not there when the server
// has no Last.fm (its /lastfm is 404).
import { authLink, keepSession, readSession, type LastfmApi } from './lastfm';
import { LASTFM_RESULT, LASTFM_SESSION, localStore } from './prefs';
import { t } from './i18n';

export const FMBOT = 'https://fm.bot/';
const RESULT_FRESH_MS = 10 * 60_000;

export interface LastfmRowDeps {
  api: Pick<LastfmApi, 'key'>;
  log(line: string): void;
  /** the page's short message */
  say(msg: string): void;
}

export class LastfmRow {
  readonly el = Object.assign(document.createElement('div'), { className: 'set lastfm', hidden: true });
  private key: string | null | undefined;      // undefined: not asked yet (or the server did not answer)
  private state: { text: () => string; cls: 'ok' | 'bad' } | null = null;

  constructor(private deps: LastfmRowDeps) {
    this.el.dataset.key = 'lastfm';
    // the popup's page (lastfm-callback.ts) keeps the session and its result: this page hears of it here
    window.addEventListener('storage', e => { if (e.key === LASTFM_RESULT) this.result(); else if (e.key?.startsWith(LASTFM_SESSION)) this.render(); });
    void this.askKey();
  }

  private async askKey() {
    try { this.key = await this.deps.api.key(); } catch (e) { this.deps.log(`[lastfm] the server did not say: ${e}`); }
    this.render();
  }

  /** what the popup did: connected, or why not */
  private result() {
    const r = localStore.getJSON(LASTFM_RESULT) as { result?: string; at?: number } | null;
    if (r !== null) localStore.remove(LASTFM_RESULT);
    if (!r || !r.at || Date.now() - r.at > RESULT_FRESH_MS) return;
    this.deps.log(`[lastfm] connect: ${r.result}`);
    const s = readSession();
    if (r.result === 'ok' && s) { this.state = null; this.deps.say(t('Last.fm connected: scrobbling as {name}.', { name: s.name })); }
    else this.state = { text: () => t('Last.fm didn\'t connect. Try again.'), cls: 'bad' };
    this.render();
  }

  /** the session key stopped working (lastfm.ts Scrobbler) */
  dropped() {
    keepSession(null);
    this.state = { text: () => t('Disconnected: the access was taken back on Last.fm.'), cls: 'bad' };
    this.render();
  }

  private connect() {
    if (!this.key) { void this.askKey(); this.deps.say(t('Last.fm can\'t be reached now. Try again in a moment.')); return; }
    this.state = null;
    const url = authLink(this.key);
    const w = window.open(url, 'lastfm', 'popup,width=480,height=720');
    if (!w) this.deps.say(t('Allow pop-ups for this site, then try again.'));
    this.render();
  }

  private disconnect() {
    keepSession(null);
    this.state = null;
    this.deps.log('[lastfm] disconnected');
    this.render();
  }

  render() {
    const r = this.el, s = readSession();
    r.hidden = this.key === null && !s;          // (a server without Last.fm; a session kept from before still shows, to disconnect it)
    const label = Object.assign(document.createElement('div'), { className: 'set-label', textContent: 'Last.fm' });
    const help = Object.assign(document.createElement('p'), { className: 'set-help', id: 'set-lastfm-help',
      textContent: t('Scrobbles the songs you hear, EA Trax and your Radio\'s, to your Last.fm profile.') });
    const ctl = Object.assign(document.createElement('div'), { className: 'set-control' });
    const b = Object.assign(document.createElement('button'), { type: 'button', className: 'btn', textContent: s ? t('Disconnect') : t('Connect') });
    b.setAttribute('aria-describedby', 'set-lastfm-help set-lastfm-state');
    b.onclick = () => s ? this.disconnect() : this.connect();
    ctl.append(b);
    const st = Object.assign(document.createElement('p'), { id: 'set-lastfm-state' });
    if (s) { st.className = 'set-state ok'; st.textContent = t('Scrobbling as {name}.', { name: s.name }); }
    else if (this.state) { st.className = `set-state ${this.state.cls}`; st.textContent = this.state.text(); }
    else st.hidden = true;
    // .fmbot: one line, a link of its own
    const bot = Object.assign(document.createElement('a'), { className: 'fmbot', href: FMBOT, target: '_blank', rel: 'noopener' });
    const name = Object.assign(document.createElement('span'), { className: 'fmbot-mark', role: 'img' });
    name.setAttribute('aria-label', '.fmbot');
    const what = Object.assign(document.createElement('span'), { className: 'fmbot-what', textContent: t('Your Last.fm stats in Discord') });
    const go = Object.assign(document.createElement('span'), { className: 'fmbot-go', textContent: t('Add') });
    bot.append(name, what, go);
    r.replaceChildren(label, help, ctl, st, bot);
  }
}
