// Claimed names on the start page (claim.ts): the line under Your name (Settings, Online) with its Claim button, and
// what Play asks about the name (shell.ts settleName):
// - Your name: as typed, the line says what the name is to this player: free (Claim; without a keyphrase, Claim
//   turns cloud saves on first), claimed by this keyphrase (until when), another player's (the digits it plays
//   under), NAME (the game's default, which nobody claims), or a change of name (Change name, once a week).  A claim
//   runs Cloudflare's check in the row when the server asks for it.
// - Play: NAME asks once for a name of the player's own; a name another player claimed asks once (per name) for
//   another; either way the game plays under the name with this browser's four digits until then.  A free name with
//   a keyphrase is claimed as the game starts: Cloudflare's check shows only when it wants a click.
// The texts and their look follow the page's own rows and dialogs (one line, coloured by its state).

import { dateLocale, t } from './i18n';
import { digitsFor, GRACE_DAYS, HOLD_DAYS, NameClaims, onlineName, readsAsSlur, turnstile, type Claimed, type Mine, type NameInfo } from './claim';

/** a day as the page writes dates: "26 Apr 2027" in the page's language */
export function day(ms: number): string {
  return new Date(ms).toLocaleDateString(dateLocale(), { day: 'numeric', month: 'short', year: 'numeric' });
}

/** what a typed name is, before the server is asked: a line and its class, or null (ask the server) */
function local(typed: string): [string, string] | null {
  if (!onlineName(typed)) return [t('3 to 15 letters, digits, - or _.'), 'bad'];
  if (readsAsSlur(typed)) return [`${t('"{value}" is not a name (it reads as a slur)', { value: typed })}.`, 'bad'];
  return null;
}

/** a claim refused, as the line says it */
export function claimError(r: Extract<Claimed, { ok: false }>, name: string): string {
  switch (r.error) {
    case 'captcha': return t('Cloudflare\'s check did not pass. Try again.');
    case 'captcha-down': return t('Cloudflare\'s check could not be reached. Try again later.');
    case 'keyphrase': return t('Your cloud saves are not on the server yet. Try again in a moment.');
    case 'taken': return t('Another player claimed {name} just now.', { name });
    case 'soon': return t('You can change your name again on {date}.', { date: day(r.next ?? Date.now()) });
    case 'busy': return t('Too many claims from this network today. Try again tomorrow.');
    case 'default': return t('{name} is the game\'s default and can\'t be claimed. Type a name of your own.', { name });
    case 'blocked': return `${t('"{value}" is not a name (it reads as a slur)', { value: name })}.`;
    case 'invalid': return t('3 to 15 letters, digits, - or _.');
    default: return t('The server did not take the claim. Try again later.');
  }
}

export interface NameRowDeps {
  claims: NameClaims;
  /** this browser's id (page-id.ts): the seed of its digits */
  seed(): string;
  /** a keyphrase is linked here */
  linked(): boolean;
  /** cloud saves turned on: the keyphrase made (null: not on, and the player was told why) */
  enable(): Promise<string | null>;
  /** a keyphrase just made, shown to keep, for the name it claimed (null: none yet) */
  showPassphrase(phrase: string, claimed: string | null): Promise<void>;
  /** the name the game takes when Your name is empty (the newest profile's) */
  profileName(): Promise<string | null>;
  /** Your name written to settings.ini */
  save(name: string): Promise<void>;
  /** the page may write the settings (the tab that owns the saves) */
  writable(): boolean;
  /** the Cloud saves section's line about the claim (null: none) */
  kept(name: string | null): void;
  say(msg: string): void;
  log(s: string): void;
}

/** The line under Your name, with its button; kept across the settings panel's re-renders (`attach` each time). */
export class NameRow {
  private info: NameInfo | null = null;
  /** the name (Online form) `info` is about, and its digits */
  private of = '';
  private alt = '';
  private claiming = false;
  /** a claim's own line (an error), until the name is typed again */
  private note: string | null = null;
  private seq = 0;
  private timer: ReturnType<typeof setTimeout> | undefined;
  private els: { input: HTMLInputElement; btn: HTMLButtonElement; line: HTMLParagraphElement; use: HTMLButtonElement } | null = null;
  /** Cloudflare's widget's box: one for every drawing of the row (the panel draws its rows again as settings change,
   *  turning cloud saves on among them, and a widget drawn into a box gone from the page never answers) */
  private readonly check = Object.assign(document.createElement('div'), { className: 'claim-check', hidden: true });

  constructor(private d: NameRowDeps) {}

  /** the row settings-ui made: its text box and control */
  attach(row: HTMLElement, input: HTMLInputElement, ctl: HTMLElement) {
    const btn = Object.assign(document.createElement('button'), { type: 'button', className: 'btn', hidden: true });
    btn.onclick = () => void this.claim();
    ctl.append(btn);
    const line = Object.assign(document.createElement('p'), { className: 'set-state free', id: 'set-roam_name-claim', hidden: true });
    line.setAttribute('aria-live', 'polite');
    const use = Object.assign(document.createElement('button'), { type: 'button', className: 'text-btn', hidden: true });
    use.onclick = () => { input.value = this.alt; input.dispatchEvent(new Event('input')); input.dispatchEvent(new Event('change')); };
    row.append(this.check, line);
    this.els = { input, btn, line, use };
    input.maxLength = 15;
    input.addEventListener('input', () => { this.note = null; clearTimeout(this.timer); this.timer = setTimeout(() => void this.ask(), 300); this.draw(); });
    this.draw();
    void this.ask();
  }

  /** the name the row is about: what is typed, else the profile's */
  private async typed(): Promise<string> {
    const v = this.els?.input.value.trim() ?? '';
    return v || (await this.d.profileName()) || '';
  }

  /** the server asked about the name typed (or the profile's) */
  async ask() {
    const typed = await this.typed();
    const name = onlineName(typed);
    const n = ++this.seq;
    if (!name || local(typed)) { this.of = name ?? ''; this.info = null; this.draw(); return; }
    const alt = await digitsFor(name, this.d.seed());
    const info = await this.d.claims.check(name, alt);
    if (n !== this.seq) return;
    this.of = name; this.alt = alt; this.info = info;
    this.d.kept(info?.mine?.name ?? null);
    this.draw();
  }

  private draw() {
    const e = this.els;
    if (!e) return;
    const { btn, line, use } = e;
    const check = this.check;
    const show = (text: string | null, cls = 'free', button: string | null = null, offer: string | null = null) => {
      line.hidden = !text;
      line.textContent = text ?? '';
      line.className = `set-state ${cls}`;
      btn.hidden = !button;
      btn.textContent = button ?? '';
      btn.disabled = this.claiming || !this.d.writable();
      use.hidden = !offer;
      if (offer) { use.textContent = t('Use {name}', { name: offer }); line.append(' ', use); }
    };
    check.hidden = !this.claiming;
    if (this.claiming) return show(t('One check that you are a person, so no one can claim names by the thousand. Once per name.'), 'free', t('Claiming'));
    const typed = e.input.value.trim();
    const bad = typed ? local(typed) : null;
    if (bad) return show(bad[0], bad[1]);
    if (this.note) return show(this.note, 'bad');
    const i = this.info;
    if (!i || this.d.claims.here === false || onlineName(typed || this.of) !== this.of) return show(null);
    const name = i.name ?? this.of, mine = i.mine;
    switch (i.state) {
      case 'default':
        return show(t('{name} is the game\'s default and can\'t be claimed. Type a name of your own.', { name }), 'env', null, this.alt);
      case 'yours':
        return show(t('Claimed with your passphrase. Yours while you play at least once every {n} days (now until {date}).', { n: HOLD_DAYS, date: day(i.until ?? 0) }), 'ok');
      case 'old':
        if (mine && mine.next <= Date.now()) return show(t('{name} was your name before your change: yours until {date}.', { name, date: day(i.until ?? 0) }), 'ok', t('Change name'));
        return show(t('{name} was your name before your change: yours until {date}.', { name, date: day(i.until ?? 0) }), 'ok');
      case 'taken':
        return show(t('Another player claimed {name}, so you play as {alt}. Type another name. Is it yours? Enter your passphrase under Cloud saves.', { name, alt: this.alt }), 'bad', null, this.alt);
      case 'free':
        if (mine && mine.next > Date.now()) return show(t('{name} is free. You can change your name again on {date}.', { name, date: day(mine.next) }));
        if (mine) return show(t('{name} is free. {old} stays yours for {n} days after the change, and your EA Messenger friends move with you. One change a week.', { name, old: mine.name, n: GRACE_DAYS }), 'free', t('Change name'));
        if (!this.d.linked()) return show(t('Not claimed: anyone can play as {name}. Claim makes you a passphrase, 8 words that hold the name and also keep your cloud saves.', { name }), 'free', t('Claim'));
        return show(t('Not claimed yet. Your passphrase claims it the next time you play with free roam or Online on.'), 'free', t('Claim'));
      default:
        return show(null);
    }
  }

  /** Claim (or Change name): cloud saves first when there is no keyphrase (the keyphrase shown to keep after the
   *  claim, for the name too), Cloudflare's check when the server asks */
  private async claim() {
    if (!this.els || this.claiming || !this.d.writable()) return;
    const name = onlineName(await this.typed());
    if (!name) return;
    const phrase = this.d.linked() ? null : await this.made();
    if (phrase === undefined) return;
    if (phrase && this.info?.state !== 'free') { await this.d.showPassphrase(phrase, null); return; }
    const mine = await this.claimNow(name);
    if (phrase) await this.d.showPassphrase(phrase, mine);
  }

  /** a keyphrase made for the claim (cloud saves on), the name asked about again; undefined: none */
  private async made(): Promise<string | undefined> {
    this.d.log('[app] claim: cloud saves first (no keyphrase here)');
    const phrase = await this.d.enable();
    if (!phrase) return undefined;
    await this.ask();
    return phrase;
  }

  /** the claim itself: the name claimed, or null (the line says why) */
  private async claimNow(name: string): Promise<string | null> {
    this.claiming = true; this.note = null; this.draw();
    let captcha: string | null = null;
    if (this.info?.captcha) {
      this.check.replaceChildren();
      captcha = await turnstile(this.info.captcha, this.check);
      if (!captcha) { this.claiming = false; this.note = t('Cloudflare\'s check did not pass. Try again.'); this.draw(); return null; }
    }
    const r = await this.d.claims.claim(name, captcha);
    this.claiming = false;
    if (r.ok) {
      this.d.log('[app] claim: your name claimed with this browser\'s keyphrase');
      if (this.els?.input.value.trim() !== r.mine.name) await this.d.save(r.mine.name);
      this.d.say(t('Claimed: {name}.', { name: r.mine.name }));
      await this.ask();
      return r.mine.name;
    }
    this.d.log(`[app] claim: your name not claimed (${r.error})`);
    this.note = claimError(r, name);
    this.draw();
    return null;
  }
}

// ---- Play ----

export interface PlayNameDeps {
  claims: NameClaims;
  seed(): string;
  linked(): boolean;
  /** the page's question box: 'ok', 'cancel' or '' (Esc) */
  choose(title: string, body: Node, ok: string, cancel: string): Promise<string>;
  save(name: string): Promise<void>;
  log(s: string): void;
}

const asked = (k: string) => { try { return localStorage.getItem(k) === '1'; } catch { return false; } };
const ask = (k: string) => { try { localStorage.setItem(k, '1'); } catch { /* storage off: asked again */ } };
const para = (text: string) => Object.assign(document.createElement('p'), { textContent: text });

/**
 * The name this game plays under, by the claims: `typed` is Your name, or the profile's.  null: as it is (no claims
 * on this server, no answer, or nothing to change).  NAME and a name another player claimed play under this
 * browser's digits, after a question the first time (per save folder for NAME, per name for a claimed one); a free
 * name with a keyphrase here is claimed (`quiet`: Cloudflare's check where it wants a click).
 */
export async function settleName(d: PlayNameDeps, typed: string, saveDir: string, quiet: (name: string, site: string) => Promise<string | null>): Promise<string | null> {
  const name = onlineName(typed);
  if (!name || readsAsSlur(typed)) return null;
  const alt = await digitsFor(name, d.seed());
  const info = await d.claims.check(name, alt);
  if (!info) return null;
  d.log(`[app] your name: ${info.state === 'yours' || info.state === 'old' ? 'claimed with this browser\'s keyphrase' : info.state === 'taken' ? 'claimed by another player' : info.state}`);
  switch (info.state) {
    case 'yours':
    case 'old':
      return name;
    case 'default': {
      const k = `nfsu2-name-asked:${saveDir}`;
      if (asked(k)) return alt;
      ask(k);
      return await pick(d, t('Pick your name'),
        [para(t('Other players see your name over your car and in the Online menu. {name} is the game\'s default and many players have it, so pick one of your own.', { name }))], alt, quiet) ?? alt;
    }
    case 'taken': {
      const k = `nfsu2-name-claimed:${name.toLowerCase()}`;
      if (asked(k)) return alt;
      ask(k);
      return await pick(d, t('{name} is claimed', { name }),
        [para(t('Another player claimed the name {name}. Pick another, or play as {alt} this time.', { name, alt }))], alt, quiet,
        para(t('Is {name} yours? Enter your passphrase under Cloud saves to get it back here.', { name }))) ?? alt;
    }
    case 'free':
      if (d.linked() && !info.mine) await claimQuietly(d, name, info, quiet);
      return name;
    default:
      return null;
  }
}

/** after a claim at Play that did not go through (the check failed, Not now, a refusal), Play tries again only this
 *  much later: a check that hangs costs its 15 s once, a player who said Not now is not asked at every Play; Claim
 *  under Your name works any time */
const RETRY_MS = 7 * 86_400_000;

/** a free name claimed at Play, Cloudflare's check quiet unless it wants a click */
export async function claimQuietly(d: PlayNameDeps, name: string, info: NameInfo, quiet: (name: string, site: string) => Promise<string | null>): Promise<Mine | null> {
  const k = `nfsu2-claim-tried:${name.toLowerCase()}`;
  try { if (Date.now() - Number(localStorage.getItem(k) ?? 0) < RETRY_MS) return null; } catch { /* storage off: tried */ }
  const later = () => { try { localStorage.setItem(k, String(Date.now())); } catch { /* storage off */ } };
  const captcha = info.captcha ? await quiet(name, info.captcha) : null;
  if (info.captcha && !captcha) { d.log('[app] claim at Play: Cloudflare\'s check did not pass (not claimed; Play tries again in a week)'); later(); return null; }
  const r = await d.claims.claim(name, captcha);
  d.log(r.ok ? '[app] claim at Play: your name claimed with this browser\'s keyphrase' : `[app] claim at Play: not claimed (${r.error}; Play tries again in a week)`);
  if (!r.ok) later();
  return r.ok ? r.mine : null;
}

/** a question for a name of the player's own: the box (filled with `alt`) checked as it is typed; the name, or null */
async function pick(d: PlayNameDeps, title: string, before: Node[], alt: string, quiet: (name: string, site: string) => Promise<string | null>, after?: Node): Promise<string | null> {
  const body = document.createElement('div');
  const row = document.createElement('p'); row.className = 'dlg-field';
  const label = Object.assign(document.createElement('label'), { className: 'set-label', htmlFor: 'claim-ask-name', textContent: t('Your name') });
  const f = Object.assign(document.createElement('input'), { type: 'text', id: 'claim-ask-name', value: alt, maxLength: 15, autocomplete: 'off', spellcheck: false });
  row.append(label, f);
  const line = Object.assign(document.createElement('p'), { className: 'hint claim-line' });
  line.setAttribute('aria-live', 'polite');
  body.append(...before, row, line);
  if (after) body.append(after);
  const got: { info: NameInfo | null } = { info: null };
  let seq = 0;
  const check = async () => {
    const n = ++seq, typed = f.value.trim(), bad = local(typed);
    if (bad) { line.textContent = bad[0]; line.dataset.state = 'bad'; f.setCustomValidity(bad[0]); return; }
    const name = onlineName(typed)!;
    const i = await d.claims.check(name);
    if (n !== seq) return;
    got.info = i;
    const [text, state] = !i ? ['', ''] : i.state === 'taken' ? [t('Another player claimed {name}.', { name }), 'bad']
      : i.state === 'default' ? [t('{name} is the game\'s default and can\'t be claimed. Type a name of your own.', { name }), 'env']
      : i.state === 'yours' || i.state === 'old' ? [t('{name} is yours.', { name }), 'ok'] : [t('{name} is free.', { name }), 'free'];
    line.textContent = text; line.dataset.state = state;
    f.setCustomValidity(state === 'bad' || state === 'env' ? text : '');
  };
  let timer: ReturnType<typeof setTimeout> | undefined;
  f.oninput = () => { clearTimeout(timer); timer = setTimeout(() => void check(), 250); };
  void check();
  const answer = d.choose(title, body, t('Use this name'), t('Not now'));
  f.focus(); f.select();
  const how = await answer;
  if (how !== 'ok') { d.log('[app] your name: not now (plays with this browser\'s digits)'); return null; }
  await check();
  const name = onlineName(f.value.trim());
  const info = got.info;
  if (!name || local(f.value.trim()) || !info || info.state === 'taken' || info.state === 'default') return null;
  await d.save(name);
  d.log('[app] your name: one of the player\'s own picked at Play');
  if (info.state === 'free' && d.linked() && !info.mine) await claimQuietly(d, name, info, quiet);
  return name;
}

/** Cloudflare's check for claiming `name` at Play: its dialog (#claim-check) stays out of sight unless the check
 *  wants a click; 15 s without an answer or a click, or Not now: no claim this time */
export async function quietCheck(name: string, site: string): Promise<string | null> {
  const dlg = document.querySelector<HTMLDialogElement>('#claim-check');
  if (!dlg) return null;
  dlg.querySelector('h2')!.textContent = t('Claiming {name}', { name });
  dlg.querySelector('.claim-about')!.textContent = t('One check that you are a person, so no one can claim names by the thousand. Once per name.');
  const box = dlg.querySelector<HTMLDivElement>('.claim-check')!;
  const no = dlg.querySelector<HTMLButtonElement>('.claim-no')!;
  no.textContent = t('Not now');
  const stop = new AbortController();
  no.onclick = () => stop.abort();
  dlg.oncancel = e => { e.preventDefault(); stop.abort(); };
  dlg.classList.add('quiet');
  dlg.showModal();
  let shown = false;
  const timer = setTimeout(() => { if (!shown) stop.abort(); }, 15_000);
  const token = await turnstile(site, box, { quiet: true, stop: stop.signal, asks: () => { shown = true; dlg.classList.remove('quiet'); no.focus(); } });
  clearTimeout(timer);
  dlg.close();
  box.replaceChildren();
  return token;
}
