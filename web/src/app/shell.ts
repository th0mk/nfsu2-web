// The start page and everything around a game run: the checks (only a
// problem is shown), loading the module with progress, the settings, saves
// and controls on the page, Play -> the game (in the window, or full screen
// if settings.ini asks) -> its end -> back here.
//
// Page address parameters (test aids: a player needs none, and the page takes
// them only from a development server, one started with --dev, which serves
// /test/dev.json; elsewhere they are ignored and the page says so, so a link
// cannot change a player's game: the audit's PAGE-1 and PRIV-2):
//   env=NAME=value   (repeatable) the "environment": wins over settings.ini
//                    (never the mounts or the save overlay's own files: HOST_FIXED, PATH_VARS)
//   save=<name>      OPFS folder of the save overlay (default "save"; never "cache", the game data's)
//   fresh            start with an empty save folder (only for save != "save")
//   reg=<url>        registry seed for this run instead of the built-in one
//   script=<url>     autotest input script (NFSU2_INPUT_SCRIPT)
//   artifacts=<url>  where the autotest's frame dumps are PUT (the server's /artifacts/)
//   dumpscale=<f>    their scale
//   rendercheck=W,D  the render check's warm-up and verdict, in seconds of driving (rendercheck.ts)
// While any of them is given, the start page says whether it is in effect.

import registryDefaults from '../../../app/registry-defaults.txt';
import { browserName, check, checkBrowser, checkGpu, checkIsolation, checkStorage, fmtBytes, type Check } from './checks';
import { codeOf, dateLocale, language as pageLanguage, lastLanguage, LANGUAGES, listOf, live, LOCALE, N, NATIVE, onLanguage, setLanguage, t, T, tn, tvars, type Msg } from './i18n';
import { loadGame, serverRev, type Loaded } from './loader';
import { claimInstance } from './instance';
import { LogRing, downloadBlob, stamp } from './logs';
import { LastGameKeeper, lastGameEnd, lastGameLine, lastGameToOffer, type LastGame } from './lastgame';
import { browserBrief, deviceBrief } from './devicelog';
import { canFullscreen, enterFullscreen, exitFullscreen, fromHomeScreen, homeScreenInstead, isFullscreen, keepScreenOn, measureRefresh, settledSize, trackCanvasSize } from './display';
import { deviceDefaults, isPhone, markTouch, noPinchZoom, padSeen, phoneScreen, touchFirst } from './device';
import { phoneSeed, phoneUpgrade } from './phonegraphics';
import { markPad, padPress, pageDialog } from './padnav';
import { controlsSheet, type ControlsSheet } from './controls';
import { TouchControls } from './touch';
import { TextField } from './textentry';
import { PauseHint } from './pausehint';
import { DriftPanel } from './drift';
import { PhotoMode } from './photo';
import type { PadPage } from '../input/gamepad';
import type { NetQueue, NetRemoved } from '../net';
import { checkSetting, HOST_FIXED, pageOnly, resolveEnv, settingsUpgrade, TEMPLATE, type Resolved } from './settings';
import { exists, filesOf, listProfiles, PROFILES_DIR, readText, removeSaveDir, writeFile } from './savefs';
import { turnOnAutosave } from './profile';
import { crewControl, putKey, SettingsPanel, showCrewCounts } from './settings-ui';
import { askCrews, queueText, roomHash, serverAnswers, watchPlayers, type PlayerCounts } from './players';
import { crewList, crewsOf, crewsValue, rgbsValue, rgbWords, type Crew } from './crews';
import { privacy, SCRUB_NOTE } from './privacy';
import { SavesPanel } from './saves-ui';
import { saveScreenshot } from './shots';
import { forgetLink, VaultPanel } from './vault-ui';
import { Radio, MODES, radioWasOn, type Mode } from '../radio';
import { canPickMusic, pickMusic } from '../radio-folder';
import { RadioLink } from './radio-link';
import { FpsOverlay, FPS_MODES, type FpsMode } from './overlay';
import { LobbyPanel } from './lobby';
import { ICON as CHAT_ICON, QuickChat } from './quickchat';
import { groupsOf, phrasesOf } from './phrases';
import { GameSession } from './game';
import { AwayCheck, AWAY_LIMITS, type AwayLimits } from './away';
import { PlayTime, SMS_AFTER_MS } from './playtime';
import { pageId } from './page-id';
import { RenderCheck, RENDER_CHECK_TIMES, type RenderCheckTimes } from './rendercheck';
import { moviesKept, Predownload } from './predownload';
import { movieCodec } from '../host/movie';
import { texSplit } from '../gpu/index';

type Phase = 'boot' | 'ready' | 'error' | 'waiting' | 'starting' | 'playing' | 'exited';

const $ = <T extends HTMLElement = HTMLElement>(sel: string) => document.querySelector<T>(sel)!;

// the page address's test aids, once boot knows whether the server is a development server
const TEST_PARAMS = ['env', 'save', 'fresh', 'reg', 'script', 'artifacts', 'dumpscale', 'away', 'rendercheck', 'playtime'];
const addr = new URLSearchParams(location.search);
let q = new URLSearchParams();
let SAVE = 'save';
let ENVIRONMENT: Record<string, string> = {};
/** the test aids the page address gives (taken on a development server only); their names, for the start page */
function takeParams(dev: boolean): string[] {
  const given = TEST_PARAMS.filter(k => addr.has(k));
  if (!given.length || !dev) return given;
  q = addr;
  // never the game data's own OPFS folder (savefs keeps names to one segment)
  const save = (q.get('save') || 'save').replace(/[^A-Za-z0-9._-]/g, '_');
  SAVE = save === 'cache' || /^\.+$/.test(save) ? 'save-test' : save;
  ENVIRONMENT = Object.fromEntries(q.getAll('env').filter(e => e.includes('='))
    .map(e => [e.slice(0, e.indexOf('=')), e.slice(e.indexOf('=') + 1)])
    // the mounts and the save overlay's own files stay the page's (settings.ts HOST_FIXED, PATH_VARS)
    .filter(([k, v]) => !HOST_FIXED.has(k) && !pageOnly(k, v, false)));
  return given;
}
/** a development server (--dev) serves the test pages' folder, and in it /test/dev.json; no other server does
 *  (the server never serves /test/ without --dev).  At most 3 s: a server that does not answer is no dev server. */
async function devServer(): Promise<boolean> {
  try {
    const r = await fetch('/test/dev.json', { cache: 'no-store', signal: AbortSignal.timeout(3000) });
    return r.ok && (await r.json())?.dev === true;
  } catch { return false; }
}

const log = new LogRing(8 << 20);
log.onLine = s => { privacy.learn(s); };    // the names a downloaded log must not carry (privacy.ts)
let phase: Phase = 'boot';
let loaded: Loaded | null = null;
let displayHz = 60;
// NFSU2_DISPLAY_HZ ("fps = auto" follows it): Play waits for the measurement,
// at most 2 s (a hidden tab has no animation frames)
let refreshMeasured: Promise<void> = Promise.resolve();
let session: GameSession | null = null;
let hudWide = true;                      // the running game's HUD is edge-anchored (hud = wide), not 4:3
let solidCars: boolean | null = null;    // the running game's free roam has solid cars (null: no free roam)
let roamCrews: Crew[] | null = null;     // the running game's crews: as it started, then as the menu changed them (null: no free roam)
let crewDraft = '';                      // the crew name being typed in the menu
let popCounts = new Map<number, number>();   // how many are in free roam in each of those crews, by hash (askPopCounts)
let popAsk = 0;                          // the popup's questions so far: only the newest one's answer shows
let away: AwayCheck | null = null;
let playTime: PlayTime | null = null;
let renderCheck: RenderCheck | null = null;
let lastExit: { code: number; seconds: number } | null = null;
let keeper: LastGameKeeper;              // the running game's log, copied where a page Safari stops cannot lose it
let lastGame: LastGame | null = null;    // the last game's copy, when that game did not get to its end (lastgame.ts)
let hiddenAt = 0;                        // when the page was hidden during a game (0: shown)
const checks = new Map<string, Check>();
let instance: ReturnType<typeof claimInstance>;
let settingsText: string | null = null;
let starting = false;                    // Play pressed, the session not there yet
let fatal: { what: string; code?: string } | null = null;   // why the running game had to be stopped (English, for t())
// the last game lost its graphics (GPU-LOST): Safari's WebGPU gives this page no adapter any more, so Play reloads it
let gpuGone = false;
let fps: FpsOverlay;
let statusAt: { memory: WebAssembly.Memory; ptr: number } | null = null;   // the running game's status block
let radioLink: RadioLink | null = null;  // the radio's words of the running game, once the game says it has them
const radioPort = () => radioLink ??= statusAt ? RadioLink.open(statusAt.memory, statusAt.ptr) : null;
let radio: Radio | null = null;          // made at the first click on Radio: a player who never uses it runs none of it
const theRadio = () => radio ??= new Radio(radioPort, l => log.add(`[radio] ${l}`));
let lobbyPanel: LobbyPanel;
let quickChat: QuickChat;                // free roam's fixed phrases (quickchat.ts)
let settings: SettingsPanel;
let players: { now(): void; last(): PlayerCounts | null; fresh(ms: number): Promise<PlayerCounts | null> } | null = null;
let saves: SavesPanel;
let vault: VaultPanel;
let download: Predownload;
/** the server does not answer: the page and the module came from this browser's copies (sw.ts), and a game
 *  runs from the game data on this PC */
let offline = false;
/** the profile the running (or last) game saved last, when, and how often it saved one */
let saved: { name: string; at: number; count: number } | null = null;

// ------------------------------------------------------------- progress --
function setProgress(f: number) {
  const v = Math.max(0, Math.min(1, f));
  $('#progress-fill').style.width = `${(v * 100).toFixed(1)}%`;
  $('#progress').setAttribute('aria-valuenow', String(Math.round(v * 100)));
}

// --------------------------------------------------------------- status --
/** the headline and the line under it (in the page's language, again when it changes); code: a problem's */
function setPhase(p: Phase, title: Msg, sub: Msg, code?: string) {
  phase = p;
  document.body.dataset.phase = p;
  live($('#status'), title);
  setSub(sub, code);
  updatePlay();
}
/** the line under the headline alone; subSeq counts what it showed (an answer that comes later may add to it) */
let subSeq = 0;
function setSub(sub: Msg, code?: string) {
  subSeq++;
  live($('#status-sub'), sub, code);
}
function setCheck(c: Check) {
  checks.set(c.id, c);
  renderProblems();
  evaluate();
}
const ORDER = ['browser', 'isolation', 'gpu', 'storage', 'module'];
let problemsShown = '';
const problemsLogged = new Set<string>();
/** a check's label and detail in the page's language */
const label = (c: Check) => tvars(c.tl ?? c.label, c.v), detail = (c: Check) => tvars(c.td ?? c.detail, c.v);
/** Only what is wrong (a failed check) or worth knowing (another browser, a GPU without BC); the storage warning goes with
 *  the saves.  Each with its code, and in the log once, in English. */
function renderProblems() {
  const shown = ORDER.map(id => checks.get(id))
    .filter((c): c is Check => !!c && (c.state === 'fail' || (c.state === 'warn' && (c.id === 'browser' || c.id === 'gpu'))));
  const key = pageLanguage() + '|' + shown.map(c => `${c.id}:${c.state}:${c.detail}`).join('|');
  if (key !== problemsShown) {          // the list is a live region: rebuilt only when it changes
    problemsShown = key;
    $('#problems').replaceChildren(...shown.map(c => {
      const li = document.createElement('li'); li.dataset.id = c.id; li.className = c.state;
      const s = document.createElement('strong'); s.textContent = label(c);
      const p = document.createElement('p'); p.textContent = detail(c);
      if (c.code) p.append(codeOf(c.code));
      li.append(s, p);
      return li;
    }));
  }
  const st = checks.get('storage');
  for (const c of [...shown, ...(st?.state === 'warn' ? [st] : [])]) {
    const k = `${c.id}:${c.state}:${c.detail}`;
    if (!problemsLogged.has(k)) { problemsLogged.add(k); log.add(`[app] ${c.state === 'fail' ? 'problem' : 'warning'} ${c.code ?? c.id}: ${c.label}: ${c.detail}`); }
  }
  const note = $('#storage-note');
  note.hidden = st?.state !== 'warn';
  if (st?.state === 'warn') live(note, () => detail(st), st.code); else live(note, () => '');
}
/** No graphics since the last game's went (GPU-ADAPTER-AFTER-LOST): asked again every 10 s while the page shows, as the
 *  browser may give them back after a while (Chrome: two minutes); the log says when they came back. */
function recheckGpu(since = performance.now()) {
  setTimeout(async () => {
    const c = document.visibilityState === 'visible' ? await checkGpu(true) : null;
    if (!c || c.code === 'GPU-ADAPTER-AFTER-LOST') { recheckGpu(since); return; }
    log.add(`[app] the graphics are back after ${((performance.now() - since) / 1000).toFixed(0)} s`);
    setCheck(c);
  }, 10_000);
}
function blocking(): Check | undefined { return ORDER.map(id => checks.get(id)).find(c => c?.state === 'fail'); }

/** Decide the start page's headline from what is known so far. */
function evaluate() {
  if (phase === 'starting' || phase === 'playing') return;
  const bad = blocking();
  if (bad) {
    setPhase('error', T('Not ready'), bad.id === 'module' ? T('The game did not load. It is tried again by itself, or press Retry.') : T('Fix the problem below, then reload this page.'));
    return;
  }
  if (!instance.owner) {
    setPhase('waiting', T('Open elsewhere'), T('This browser is already running the game in another tab. Switch to that tab, or close it: this page takes over when it does.'));
    return;
  }
  const pending = ORDER.map(id => checks.get(id)).find(c => c?.state === 'pending');
  if (!loaded) {
    // downloading (again, after a failure): the headline says so, the progress fills in the line below
    if (pending?.id === 'module' && phase !== 'boot' && phase !== 'exited') setPhase('boot', T('Loading'), T('Loading the game.'));
    return;
  }
  if (pending) { if (phase !== 'exited') setPhase('boot', T('Loading'), () => `${label(pending)}: ${detail(pending)}`); return; }
  if (phase === 'exited') { updatePlay(); return; }
  const press = touchFirst() ? N('Press Play to start.') : N('Press Play or Enter to start.');
  setPhase('ready', T('Ready'), offline ? () => t('Offline: the game runs from this PC, without free roam or Online. {press}', { press: t(press) }) : T(press));
}

function updatePlay() {
  const ok = !!loaded && instance.owner && !blocking() && !starting && (phase === 'ready' || phase === 'exited');
  $<HTMLButtonElement>('#play').disabled = !ok;
  $('#retry').hidden = checks.get('module')?.state !== 'fail';
}

// ------------------------------------------------------------- settings --
async function loadSettings(): Promise<string | null> {
  let text = await readText(SAVE, 'settings.ini');
  if (text === null && instance.owner) {
    text = TEMPLATE;
    await writeFile(SAVE, 'settings.ini', text);
    log.add(`[app] settings: wrote template /save/settings.ini`);
  } else if (text !== null && instance.owner) {
    const up = settingsUpgrade(text);
    if (up) { text = up.text; await writeFile(SAVE, 'settings.ini', text); log.add(`[app] settings: added the new options ${up.added.join(', ')} to /save/settings.ini (commented out, defaults unchanged)`); }
  }
  settingsText = text;
  // who plays on this server, asked again with this file's room and crews (the first question went without them)
  players?.now();
  return text;
}
/** this device's own defaults (a phone's: device.ts), below settings.ini and the page address */
const DEVICE = deviceDefaults();
function resolved(host: Record<string, string> = {}): Resolved { return resolveEnv(settingsText, ENVIRONMENT, host, DEVICE); }
/** the game's language (runtime/language.c): which language pack the page fetches ahead and counts */
const language = (r: Resolved = resolved()) => (r.env.NFSU2_LANGUAGE ?? 'english').toLowerCase();
/** voices option ('dubbed' or 'original'): original skips language pack speech/movies */
const voices = (r: Resolved = resolved()) => (r.env.NFSU2_VOICES ?? 'dubbed').toLowerCase();
/** the texture pack picked (runtime/web/texrep_web.c), '' for none: the one the page fetches ahead and counts */
const texpack = (r: Resolved = resolved()) => { const v = (r.env.NFSU2_UI_TEXTURES ?? '').toLowerCase(); return v === 'original' ? '' : v; };

// ----------------------------------------------------------------- play --
function stageCanvas(): HTMLCanvasElement {
  const stage = $('#stage');
  stage.querySelector('canvas')?.remove();
  const c = document.createElement('canvas');
  c.id = 'game'; c.tabIndex = 0; c.setAttribute('aria-label', t('The game'));
  stage.prepend(c);
  return c;
}

let untrack: (() => void) | null = null;

/**
 * The first Play asks whether to play free roam with others (off by default: with it on, everyone on the server
 * sees the player's name and status).  The answer goes into settings.ini as roam = on or off, so it is asked
 * once; Not now and Esc are off.  Not asked when the file or the page address sets it already, on a test page
 * (the address's test settings), offline or with network = off.  The name shows only when there is one (Your
 * name, else the newest career profile's): a new player has none yet, the game asks for it before free roam.
 * While the server's free roam is busy (its newest /players says roamBusy: fewer links a player) it is not asked
 * and nothing is written, so a later Play asks once free roam has room again.  While the server is full it is asked,
 * and says that with it on the game waits in line for a place (and plays alone until then).  True: it was asked.
 * ASK_ROAM false: never asked, and free roam stays off until the player turns it on under Online.
 */
const ASK_ROAM = true;
async function askRoam(): Promise<boolean> {
  const r = resolved();
  if (!ASK_ROAM || r.source.NFSU2_ROAM || TEST_PARAMS.some(k => q.has(k)) || offline || /^(off|0|none)$/i.test(r.env.NFSU2_NET?.trim() ?? '')) return false;
  if (players?.last()?.roamBusy === true) { log.add('[app] free roam with others: not asked, the server\'s free roam is busy (asked at a later Play)'); return false; }
  const name = (r.source.NFSU2_ROAM_NAME === 'settings.ini' ? r.env.NFSU2_ROAM_NAME : '') || (await newestProfile())?.name || '';
  const p = (text: string) => Object.assign(document.createElement('p'), { textContent: text });
  const body = document.createElement('div');
  body.append(p(t('Other players on this server can drive in your career\'s free roam with you. You see their cars with their names over them, they see yours, and you can challenge each other to outruns. Races, traffic and money stay your own.')),
    p(t('With this on, everyone on this server sees your picked name and whether you\'re driving or in the menus. Players near you also see your car and where it is.')));
  const c = players?.last();
  if (c?.full === true) {
    const n = Number.isInteger(c.queue) && c.queue! > 0 ? c.queue! : 0;
    const turn = t('With this on, you wait in line too and drive alone until it is your turn; then the others join you by themselves.');
    body.append(p(`${n ? tn(n, 'The server is full right now: {n} player is waiting in line for a place.', 'The server is full right now: {n} players are waiting in line for a place.')
      : t('The server is full right now.')} ${turn}`));
  }
  let field: HTMLInputElement | null = null;
  if (name) {
    const row = document.createElement('p'); row.className = 'dlg-field';
    const label = Object.assign(document.createElement('label'), { className: 'set-label', htmlFor: 'roam-ask-name', textContent: t('Your name') });
    const f = field = Object.assign(document.createElement('input'), { type: 'text', id: 'roam-ask-name', value: name, maxLength: 24, autocomplete: 'off', spellcheck: false });
    // a name the setting refuses keeps the question open (Not now and Esc still close it)
    f.oninput = () => { const v = f.value.trim(), c = v ? checkSetting('roam_name', v) : null; f.setCustomValidity(c && !c.ok ? t('{Error}.', { Error: tvars(c.why, c.v) }) : ''); };
    // Enter in the field turns it on (the form's first button, which Enter would press, is Not now)
    f.onkeydown = e => { if (e.key === 'Enter') { e.preventDefault(); $<HTMLButtonElement>('#confirm-ok').click(); } };
    row.append(label, f);
    body.append(row);
  }
  body.append(p(t('You can change this any time under Online on the start page.')));
  const answer = confirmBox(t('Free roam with others?'), body, t('Turn on'), false, '', t('Not now'));
  $<HTMLButtonElement>('#confirm-ok').focus();
  const on = await answer;
  let text = putKey(settingsText ?? TEMPLATE, 'roam', on ? 'on' : 'off').text;
  const typed = field?.value.trim() ?? name;
  if (on && typed !== name) text = putKey(text, 'roam_name', typed || null).text;
  await writeFile(SAVE, 'settings.ini', text);
  settingsText = text;
  void settings.refresh();
  log.add(`[app] free roam with others: ${on ? 'on' : 'off'} (asked at the first Play${c?.full === true ? ', the server full' : ''})${on && typed !== name ? `, name ${typed || '(the profile\'s)'}` : ''}`);
  return true;
}

/**
 * The fifth Play in this browser (four games played here, vault-ui.ts played) of a player with a profile here and no
 * cloud saves asks to turn them on: the saves live only in this browser, and browsers clear a site's data now and then.
 * Asked once (Not now and Esc are no); not at the Play that asked about free roam (the next one asks), on a test page,
 * offline, or where the server keeps no cloud saves.  Turned on, the keyphrase is shown to keep (copy, download)
 * before the game starts; it failed, the reason.
 */
async function askCloud() {
  if (testPage() || offline || !vault.offerable() || !await newestProfile()) return;
  vault.offered();
  const p = (text: string) => Object.assign(document.createElement('p'), { textContent: text });
  const body = document.createElement('div');
  body.append(p(t('Your career is saved only in this browser. Browsers sometimes clear a website\'s data, for example when the device runs low on space or when browsing data is cleared, and your saves go with it.')),
    p(t('Cloud saves keep a copy on this server under a keyphrase that only you have. With it you get your saves back, here or in another browser.')),
    p(t('You can change this any time under Cloud saves on the start page.')));
  const answer = confirmBox(t('Turn on cloud saves?'), body, t('Turn on'), false, '', t('Not now'));
  $<HTMLButtonElement>('#confirm-ok').focus();
  const on = await answer;
  log.add(`[app] cloud saves: ${on ? 'turned on' : 'not now'} (asked at Play)`);
  if (!on) return;
  const got = await vault.enable();
  if ('why' in got) {
    await confirmBox(t('Cloud saves are not on'), got.why ? t('They could not be turned on: {error}. Try again under Cloud saves on the start page.', { error: got.why })
      : t('They could not be turned on. Try again under Cloud saves on the start page.'), t('Continue'), false, '', '');
    return;
  }
  const box = Object.assign(document.createElement('p'), { className: 'keyphrase', textContent: got.phrase });
  const copy = Object.assign(document.createElement('button'), { type: 'button', className: 'text-btn', textContent: t('Copy') });
  copy.onclick = async () => {
    try { await navigator.clipboard.writeText(got.phrase); copy.textContent = t('Copied.'); }
    catch { getSelection()?.selectAllChildren(box); }
  };
  const file = Object.assign(document.createElement('button'), { type: 'button', className: 'text-btn', textContent: t('Download keyphrase') });
  file.onclick = () => vault.download();
  const row = document.createElement('div'); row.className = 'row keyphrase-do';
  row.append(copy, file);
  const kept = document.createElement('div');
  kept.append(p(t('Keyphrase made. Keep it safe: it is the only way back to these saves.')), box, row,
    p(t('To load your saves, enter it under Cloud saves on the start page.')));
  const shown = confirmBox(t('Your keyphrase'), kept, t('Continue'), false, '', '');
  $<HTMLButtonElement>('#confirm-ok').focus();
  await shown;
}

/**
 * A full server (the newest /players, asked again at Play): a game that takes a place at once (free roam on) waits in
 * line for it.  The player picks: wait in line (the game starts, alone until it is their turn; the default, also for
 * Esc), or play offline (this game only, without free roam or Online; settings.ini stays as it is).  True: offline.
 * Not asked when the game would not take a place at once (free roam off: the LAN and Online screens wait in line
 * themselves, the line over the game says where), offline mode, network = off or a test page, nor right after the
 * free-roam question (askRoam), which says the server is full itself.
 */
async function askFull(c: PlayerCounts | null): Promise<boolean> {
  const r = resolved();
  if (!c?.full || offline || TEST_PARAMS.some(k => q.has(k)) || r.env.NFSU2_ROAM !== '1' || /^(off|0|none)$/i.test(r.env.NFSU2_NET?.trim() ?? '')) return false;
  const n = Number.isInteger(c.queue) && c.queue! > 0 ? c.queue! : 0;
  const p = (text: string) => Object.assign(document.createElement('p'), { textContent: text });
  const body = document.createElement('div');
  body.append(p(n ? tn(n, 'Every place on this server is taken, and {n} player is waiting in line for one.', 'Every place on this server is taken, and {n} players are waiting in line for one.')
      : t('Every place on this server is taken.')),
    p(t('Wait in line: the game starts now and you play alone. You join free roam and Online by yourself when it is your turn; the top of the screen shows your place in line.')),
    p(t('Play offline: this game runs without free roam or Online, and waits for nothing. Your settings stay as they are.')));
  const answer = choiceBox(t('The server is full'), body, t('Wait in line'), t('Play offline'));
  $<HTMLButtonElement>('#confirm-ok').focus();
  const alone = await answer === 'cancel';
  log.add(`[app] the server is full${n ? ` (${n} in line)` : ''}: ${alone ? 'this game plays offline' : 'the game waits in line for a place'} (asked at Play)`);
  return alone;
}

async function play() {
  if ($<HTMLButtonElement>('#play').disabled || session || starting) return;
  if (byPlay()) { await reloadFor(newer!, 'Play', false); return; }
  if (gpuGone) { log.add('[app] the last game lost its graphics: reloading the page (Play)'); await reload(); return; }
  if (updated) { updated = false; showNewer(); }
  starting = true; updatePlay();
  try { await startGame(); } finally { starting = false; updatePlay(); }
}

async function startGame() {
  // settings.ini as it is now (another tab or the raw editor may have changed it,
  // and the Mac app reads it at every launch); a short read, well inside the click's
  // user activation, which full screen needs (after the free-roam question, its
  // button's click is the activation).  Beside it the server's version once more
  // (a deploy since the page last asked): no game starts on a version it replaced.
  const [, rev, counts] = await Promise.all([
    Promise.race([loadSettings().catch(e => log.add(`[app] settings.ini not read again: ${e}`)), new Promise(r => setTimeout(r, 1000))]),
    offline || testPage() ? null : serverRev(1000),
    offline || testPage() ? null : players?.fresh(1000) ?? null,
  ]);
  if (rev && rev !== loaded!.info.rev && rev !== tried) {
    newer = rev; updated = false;
    // before a game ran here nothing is lost; after one, the offer with its word on the log first (the next Play reloads)
    if (phase !== 'exited') { await reloadFor(rev, 'Play', false); return; }
    log.add(`[app] the server has game ${rev} now, this page ${loaded!.info.rev}: offered at Play`);
    showNewer();
    return;
  }
  // a cloud-saves conflict waiting (nothing syncs until it is settled): settled first, or no game; before the questions
  // that write settings.ini, which the cloud's saves would replace
  const cloud = await vault.askAtPlay();
  if (cloud === 'stop') return;
  if (cloud === 'taken') await loadSettings().catch(e => log.add(`[app] settings.ini not read again: ${e}`));
  // (the free-roam question says when the server is full: answered, it is not asked again what to do about that)
  const roamAsked = await askRoam();
  if (!roamAsked) await askCloud();
  const alone = !roamAsked && await askFull(counts);
  const pre = resolved();
  // inside the click: things that need a user gesture.  The context runs at the
  // mixer's own rate (44.1 kHz), so the worklet does not resample.
  let audio: AudioContext | null = null;
  try {
    const lat = pre.env.NFSU2_AUDIO_LATENCY ?? 'interactive';
    const hint: AudioContextLatencyCategory | number = /^[0-9.]+$/.test(lat) ? Math.min(0.5, Math.max(0.001, +lat / 1000)) : lat as AudioContextLatencyCategory;
    audio = new AudioContext({ latencyHint: hint, sampleRate: 44100 });
    void audio.resume().catch(() => {});
  } catch (e) { log.add(`[app] no AudioContext: ${e}`); }
  const full = pre.env.NFSFULL !== '0';
  const stage = $('#stage');
  stage.hidden = false;
  $('#app').inert = true;             // the start page is behind the game: out of the focus order and screen readers
  const canvas = stageCanvas();
  untrack = trackCanvasSize(canvas);
  awake = keepScreenOn(logLine);
  $('#stage-menu').hidden = !touchFirst();
  setPhase('starting', T('Starting'), full && canFullscreen() ? T('Going full screen and starting the game.') : T('Starting the game.'));
  let fsNote = '';
  // an iPhone has no full screen for the page (from the Home Screen it has no browser bars to hide)
  if (full && homeScreenInstead()) fsNote = t(HOME_SCREEN);
  else if (full && canFullscreen()) {
    const fs = await enterFullscreen(stage);
    if (!fs.ok) fsNote = t('Full screen was not allowed here; {menu} has it.', { menu: menuName() });
    else if (!fs.locked && !touchFirst()) fsNote = t('Esc leaves full screen in this browser (no keyboard lock).');
  }
  const size = await settledSize(canvas);
  await Promise.race([refreshMeasured, new Promise(r => setTimeout(r, 2000))]);
  canvas.focus();

  // the keyphrase's newer saves from another browser (4 s at most), then what the Saves section is writing (a rename,
  // a restore) ends first
  if (await vault.beforePlay()) await loadSettings();
  await saves.settled();
  // the save overlay: registry seed (bundle.m app_seed_registry), test inputs
  const host: Record<string, string> = {
    HOME: '/save', NFSU2_DATA: '/data', NFSU2_SAVEDIR: '/save', NFSU2_REGFILE: '/save/registry.txt',
    NFSU2_WIDTH: String(size.w & ~1), NFSU2_HEIGHT: String(size.h & ~1), NFSU2_DISPLAY_HZ: String(displayHz),
  };
  const r = resolved(host);
  // the names this game goes by, for the log as it leaves the browser (privacy.ts)
  privacy.name(r.env.NFSU2_ROAM_NAME, 'you');
  for (const c of crewList(r.env.NFSU2_ROAM_CREW ?? '')) privacy.name(c, 'crew');
  privacy.name(r.env.NFSU2_ROAM_ROOM, 'room');
  for (const p of await listProfiles(SAVE).catch(() => [])) privacy.name(p.name, 'profile');
  // offline mode: the server does not answer.  The movies stream from it, so unless every one of them is in this
  // browser they are off for this game (a movie the game could not read would stop it).
  setReach(await serverAnswers(2000));
  // the movies in AV1 where this browser decodes it, else in VP6 (host/movie.ts; settings.ini NFSU2_MOVIE_CODEC)
  r.env.NFSU2_MOVIE_CODEC = await movieCodec(r.env);
  let offlineNote = '';
  if (offline) {
    log.add('[app] offline: the server does not answer, the game runs from this PC (no free roam or Online)');
    if (r.env.NOMOVIES !== '1' && !await moviesKept(language(r), r.env.NFSU2_MOVIE_CODEC === 'av1', voices(r)).catch(() => false)) {
      r.env.NOMOVIES = '1';
      log.add('[app] offline: not every movie is on this PC, so the movies are off for this game');
      offlineNote = t('Offline: the movies are off for this game.') + ' ';
    }
  }
  // the log is for sharing when asking for help: it names neither the server (a private server's address) nor
  // more of this browser than its major version and system (the audit's PRIV-5).  These session lines stay when
  // a long game fills the log (logs.ts keep).
  log.keep(`=== NFSU2 web, ${new Date().toISOString()}, game ${loaded!.info.rev}, server ${serverKind()} ===`);
  log.keep(`[app] save folder: OPFS ${SAVE} (/save); canvas ${size.w}x${size.h} device pixels, display ${displayHz} Hz; ${browserBrief(navigator.userAgent, navigator.maxTouchPoints, phoneScreen(navigator.maxTouchPoints))}; ${checks.get('gpu')?.label ?? 'WebGPU'}; movies in ${r.env.NFSU2_MOVIE_CODEC === 'av1' ? 'AV1' : 'VP6'}`);
  log.keep(`[app] device: ${deviceBrief({ screenW: screen.width, screenH: screen.height, dpr: devicePixelRatio, cores: navigator.hardwareConcurrency,
    memoryGB: (navigator as { deviceMemory?: number }).deviceMemory, touchPoints: navigator.maxTouchPoints, homeScreen: fromHomeScreen(), displayHz })}`);
  for (const n of r.notes) log.keep(`[app] ${n}`);
  const reg = r.env.NFSU2_REGFILE.replace(/^\/save\//, '');
  if (q.get('reg')) {
    await writeFile(SAVE, reg, await (await fetch(q.get('reg')!, { cache: 'no-store' })).text());
    log.add(`[app] registry seeded from ${q.get('reg')} (page address)`);
  } else if (!await exists(SAVE, reg)) {
    if (r.env.NFSU2_SEED_REGISTRY === '0') log.add('[app] no registry yet, the game will run its first-run check');
    else if (isPhone()) { await writeFile(SAVE, reg, phoneSeed(registryDefaults)); log.add(`[app] first launch, registry seeded with the built-in defaults and a phone's graphics (/save/${reg})`); }
    else { await writeFile(SAVE, reg, registryDefaults); log.add(`[app] first launch, registry seeded with the built-in defaults (/save/${reg})`); }
  } else if (isPhone()) {
    // a phone that started before the phone graphics (phonegraphics.ts): once
    const text = await readText(SAVE, reg);        // null: not read (it is there), left as it is for the next start
    const up = text === null ? null : phoneUpgrade(text, registryDefaults);
    if (text === null) log.add(`[app] phone graphics: /save/${reg} could not be read, left as it is`);
    else if (up) {
      await writeFile(SAVE, reg, up.text);
      log.add(up.lowered ? '[app] phone graphics: the graphics options still had the built-in defaults, now a phone\'s'
        : '[app] phone graphics: the graphics options were changed in this browser, they stay');
    }
  }
  // the game's Save option at Autosave, once per profile (settings.ini autosave, profile.ts); an input script
  // plays the game as it was recorded (on the Mac, where nothing changes the profiles) unless the page address
  // asks for autosave itself
  let autosaved: string[] = [];
  const scripted = !!q.get('script') && ENVIRONMENT.NFSU2_AUTOSAVE === undefined;
  if (scripted) log.add('[app] autosave: an input script drives this game, the profiles are left as they are');
  else if (r.env.NFSU2_AUTOSAVE !== '0') {
    try {
      const a = await turnOnAutosave(filesOf(SAVE), PROFILES_DIR);
      autosaved = a.turnedOn;
      for (const n of a.notes) log.add(`[app] autosave: ${n}`);
    } catch (e) { log.add(`[app] autosave: the profiles were not checked: ${e}`); }
  }
  if (q.get('script')) {
    await writeFile(SAVE, 'autotest-script.txt', await (await fetch(q.get('script')!, { cache: 'no-store' })).text());
    if (!('NFSU2_INPUT_SCRIPT' in r.env)) r.env.NFSU2_INPUT_SCRIPT = '/save/autotest-script.txt';
    log.add(`[app] input script ${q.get('script')}`);
  }

  // the download of all the game data writes the files the game's I/O worker opens: it waits for the game
  await download.pause();
  // the server's virtual LAN: the game's own LAN races and Online mode (network = on, the default) and free
  // roam all need it; only network = off stays off it (the game then shows its own network error)
  // (and a full server's player who chose to play offline: askFull)
  const net = !/^(off|0|none)$/i.test(r.env.NFSU2_NET?.trim() ?? '') && !alone;
  const netName = net ? (r.env.NFSU2_ROAM_NAME?.trim() || 'Player') : null;
  hudWide = !/^(43|4:3)$/i.test(r.env.NFSU2_HUD?.trim() ?? '');
  solidCars = net && r.env.NFSU2_ROAM === '1' ? r.env.NFSU2_ROAM_COLLIDE !== '0' : null;
  roamCrews = net && r.env.NFSU2_ROAM === '1' ? crewsOf(r.env.NFSU2_ROAM_CREW ?? '', r.env.NFSU2_ROAM_CREW_RGB ?? '') : null;
  crewDraft = ''; popCounts = new Map();
  fatal = null;
  quitAt = null;
  saved = null;
  try {
    // the callbacks are module-level functions, not closures made here: the
    // session's subsystems keep them (some, like a closed AudioContext, a little
    // longer than the game), and a closure from this function would keep its
    // scope, and with it this game, reachable
    session = await GameSession.start({
      module: loaded!.module, info: loaded!.info, saveDir: SAVE, env: r.env, canvas, stage, audio, displayHz, netName, notice, lobby, say, marks,
      netBack, netQueue, onSaved,
      netId: net ? pageId() : undefined, netRemoved,
      artifacts: q.get('artifacts') ?? undefined, dumpScale: q.get('dumpscale') ? +q.get('dumpscale')! : undefined,
      toggleFullscreen, padPage: PAD_PAGE, log: logLine, onFatal: stopFatal, onOtherBuild: otherBuild,
    });
  } catch (e) {
    log.add(`[app] the game could not start: ${e}`);
    void audio?.close().catch(() => {});
    endStage();
    setPhase('exited', T('Did not start'), T('The game could not start: {error}', { error: String(e) }), 'GAME-START');
    $('#log-btn').hidden = $('#report').hidden = false;
    download.resume();
    return;
  }
  setPhase('playing', T('Playing'), () => '');
  vault.played();                     // counted for the cloud saves question (askCloud)
  keeper.start(loaded!.info.rev, gameHeld);
  $('#lastgame').hidden = true;           // about the game before this one
  document.body.dataset.phase = 'playing';
  away = new AwayCheck(session, awayLimits(), awayLine, logLine);
  playTime = new PlayTime(session, playTimeLimit(), logLine);
  // a phone's render scale (device.ts): whether it has room for all its pixels, for its next game
  if (r.source.NFSU2_RENDER === 'device') renderCheck = new RenderCheck(session, r.env.NFSU2_RENDER, renderCheckTimes(), logLine);
  touch.start(session, r.env.NFSU2_FF !== '0');   // Rumble off: no tick under a finger either
  textField.start(session);
  pauseHint.start(session);
  driftPanel.start(session);
  photo.start(session);
  quickChat.start(session, net && r.env.NFSU2_ROAM === '1' && r.env.NFSU2_ROAM_SAY !== '0',
    phrasesOf(r.env.NFSU2_ROAM_SAY_PHRASES), groupsOf(r.env.NFSU2_ROAM_SAY_GROUPS));
  fps.memory = session.proc.memory;
  const st = session;
  let tries = 0;
  const waitStatus = setInterval(() => {
    if (!st.status) return;
    if (!fps.reader) { fps.reader = st.status; if (session === st) statusAt = { memory: st.proc.memory, ptr: st.status.ptr }; }
    if (session === st && radioPort()) {
      $('.menu-radio').hidden = !canPickMusic();   // the menu may have been opened before the game's status block was known
      if (radioWasOn()) void theRadio().resume();
    }
    if (radioLink || ++tries > 50) clearInterval(waitStatus);
  }, 100);
  fps.set((r.env.NFSU2_FPS_COUNTER as FpsMode) in { off: 1, on: 1, detailed: 1 } ? r.env.NFSU2_FPS_COUNTER as FpsMode : 'off');
  const key = cap(menuKey());
  const hint = offlineNote + (fsNote || (full ? (touchFirst() ? t('{key} opens the menu.', { key }) : t('{key} opens the menu. Hold Esc to leave full screen.', { key }))
    : t('{key} opens the menu: full screen, controls, quit.', { key })));
  if (autosaved.length) toast(t('Autosave is now on for {names}: the game saves by itself (Options, Gameplay, Save). {hint}', { names: listOf(autosaved), hint }), 9000);
  else toast(hint, 6000);
  void st.exited.then(code => { clearInterval(waitStatus); onExit(st, code); });
  // Chrome may keep the sound off until a click or a key press (its autoplay
  // rule); a controller button does not count, so say what does
  setTimeout(() => {
    if (session !== st || !st.audio || st.audio.state() === 'running' || r.env.NOSOUND === '1') return;
    log.add(`[app] the AudioContext is ${st.audio.state()} after Play`);
    toast(touchFirst() ? t('No sound yet: tap the game to turn it on (a controller button cannot).')
      : t('No sound yet: click the game or press a key to turn it on (a controller button cannot).'), 8000);
  }, 3000);
}

function logLine(s: string) { log.add(s); }
/** whether the server answered (the start page's /players, or the check at Play): offline mode when not */
function setReach(ok: boolean) {
  if (offline === !ok) return;
  offline = !ok;
  log.add(ok ? '[app] the server answers again' : '[app] the server does not answer: offline');
  if (!ok) settings.setStatus(T('Not connected to the server: free roam and Online need it.'));
  evaluate();
  // the page may have started from offline mode's copies while the server was away (a deploy restarts it)
  if (ok) void checkNewer();
}
/** Offline mode (sw.ts) keeps the page, its scripts, the module and the list of game files in this browser.  Asked
 *  for once the module has loaded, so a first visit downloads the module once (the copy is the HTTP cache's). */
function keepForOffline() {
  if (!('serviceWorker' in navigator) || !isSecureContext) return;
  navigator.serviceWorker.register('/sw.js').catch(e => log.add(`[app] offline mode is not available here: ${e}`));
}
/** where the server is, without its name or address */
function serverKind(): string {
  const h = location.hostname.replace(/^\[|\]$/g, '').toLowerCase();
  if (h === 'localhost' || h === '::1' || /^127\./.test(h)) return 'this machine';
  if (h.endsWith('.local') || /^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.|169\.254\.|100\.(6[4-9]|[7-9]\d|1[01]\d|12[0-7])\.|f[cd]|fe80:)/.test(h)) return 'on the LAN';
  return 'remote';
}
/** a subsystem the game cannot do without failed: the running game is stopped (what: English, for t(); code: its name) */
function stopFatal(what: string, code?: string) {
  const s = session;
  if (!s || s.ended) return;
  fatal = { what, code };
  log.add(`[app] ${what}: stopping the game`);
  s.terminate();
}
/** The renderer is of another build than the game module (game.ts onOtherBuild): this page came from offline mode's
 *  copies (sw.ts) and the module from the server, or from a server swapping its module.  The game is stopped and the
 *  page loaded again from the server: once a tab and module, while the server answers; else as any renderer that
 *  did not start. */
const OTHER_KEY = 'nfsu2-other-build';
let reloadAtExit = false;
function otherBuild(why: string) {
  const s = session, rev = loaded!.info.rev;
  if (!s || s.ended) return;
  let again = true;
  try { again = sessionStorage.getItem(OTHER_KEY) === rev; if (!again) sessionStorage.setItem(OTHER_KEY, rev); } catch { /* no storage: no reload that could repeat */ }
  if (again || offline) {
    log.add(`[app] this page is of another build than game ${rev} (${why}), ${offline ? 'and the server does not answer' : 'after a reload for that too'}`);
    stopFatal(N('the graphics could not start'), 'GPU-RENDERER');
    return;
  }
  log.add(`[app] this page is of another build than game ${rev} (${why}): stopping the game, then the page again from the server`);
  reloadAtExit = true;
  s.terminate();
}

/** what to do on an iPhone, whose browsers have no full screen for a page */
const HOME_SCREEN = N('On an iPhone a web page cannot go full screen: add this page to the Home Screen (in the Share menu) and play from its icon.');
function toggleFullscreen() {
  if (isFullscreen()) exitFullscreen();
  else if (homeScreenInstead()) toast(t(HOME_SCREEN), 8000);
  else void enterFullscreen($('#stage')).then(r => { if (!r.ok) toast(t('Full screen was not allowed.'), 4000); });
}

/** how the page's menu opens here, for the hints: F9, the ☰ button on a touch screen, Home on a controller */
function menuKey(): string {
  if (touchFirst()) return padSeen() ? t('the ☰ button or Home on the controller') : t('the ☰ button');
  return padSeen() ? t('F9 or Home on the controller') : 'F9';
}
/** "the menu (F9)", with what opens it here */
function menuName(): string {
  const key = touchFirst() ? '☰' : 'F9';
  return padSeen() ? t('the menu ({key}, or Home on the controller)', { key }) : t('the menu ({key})', { key });
}
const cap = (s: string) => s[0].toUpperCase() + s.slice(1);

/** The page in the game's language (settings.ini language, the page address's on a development server): English until
 *  the player picks another, in Settings or in the list at the top.  A tab that does not own the saves follows the file. */
function applyLanguage() {
  void setLanguage(language(resolveEnv(settingsText ?? settings.text, ENVIRONMENT, {}, DEVICE)), logLine);
  syncPicker();
}
/** the list at the top shows the language the file asks for (disabled where another tab owns the saves) */
function syncPicker() {
  const pick = $<HTMLSelectElement>('#lang-pick');
  pick.value = language(resolveEnv(settingsText ?? settings.text, ENVIRONMENT, {}, DEVICE));
  pick.disabled = !instance.owner;
  // as wide as the name shown (CSS field-sizing where the browser has it; Firefox measures it here)
  if (!CSS.supports?.('field-sizing', 'content')) {
    const g = document.createElement('canvas').getContext('2d'), cs = getComputedStyle(pick);
    if (g) { g.font = cs.font; pick.style.width = `${Math.ceil(g.measureText(pick.selectedOptions[0]?.textContent ?? '').width) + 80}px`; }
  }
}

/** the controller's share of the page while a game runs (gamepad.ts): Home, or both sticks pressed in, opens or
 *  closes the menu (not over another dialog), and an open dialog of the page takes the controller (padnav.ts) */
const PAD_PAGE: PadPage = {
  menu() {
    const d = pageDialog();
    if (d && d.id !== 'menu') return;
    openMenu();
    const m = $<HTMLDialogElement>('#menu');
    if (m.open) markPad(m);
  },
  dialog: () => !!pageDialog(),
  press: padPress,
  stick: (x, y) => quickChat.stick(x, y),
  capture: (s, pressed) => photo.pad(s, pressed),
};

let awake: (() => void) | null = null;   // the running game's Wake Lock (display.ts keepScreenOn)
let touch: TouchControls;                // the controls on the game while a finger drives (touch.ts)
let textField: TextField;               // the field for a phone's keyboard while the game asks for text (textentry.ts)
let pauseHint: PauseHint;               // the line to the page's menu under the game's pause menu (pausehint.ts)
let driftPanel: DriftPanel;             // free roam's drift mode: its score under the game's Bank panel, a note at F2 (drift.ts)
let photo: PhotoMode;                   // photo mode: F12 in the world, its panel and its keys (photo.ts)

function endStage() {
  touch.stop();
  textField.stop();
  pauseHint.stop();
  driftPanel.stop();
  photo.stop();
  quickChat.stop();
  awake?.(); awake = null;
  away?.stop(); away = null;
  playTime?.stop(); playTime = null;
  hideRemoved();
  renderCheck?.stop(); renderCheck = null;
  Object.assign(DEVICE, deviceDefaults());      // its verdict, for the next game without a reload
  exitFullscreen();
  untrack?.(); untrack = null;
  fps.stop(); fps.reader = null; fps.memory = null;
  radio?.stop('the game closed');
  radioLink = null; statusAt = null;
  lobbyPanel.clear();
  for (const id of ['#crews-pop', '#menu']) { const d = $<HTMLDialogElement>(id); if (d.open) d.close(); }
  // a Quit question still open belongs to the game that ended (its answer would
  // do nothing, and the open question would keep that game reachable)
  const ask = $<HTMLDialogElement>('#confirm'); if (ask.open && ask.dataset.about === 'quit') ask.close();
  $('#stage').hidden = true;
  $('#stage').querySelector('canvas')?.remove();
  $('#app').inert = false;
}

function onExit(s: GameSession, code: number) {
  if (session !== s) return;
  const seconds = (performance.now() - s.started) / 1000;
  log.add(`[app] the game exited with code ${code} after ${seconds.toFixed(1)} s`);
  keeper.end(code, fatal?.code ?? 'exit');
  s.terminate();
  session = null;
  quitAt = null;
  lastExit = { code, seconds };
  endStage();
  players?.now();
  download.resume();
  if (reloadAtExit) { reloadAtExit = false; setPhase('boot', T('Loading'), T('Loading the game.')); void reloadFromServer(); return; }
  $('#log-btn').hidden = false;
  // a crash, or a game stopped for want of a subsystem: where to send the log (not a normal close or a Stop)
  $('#report').hidden = !fatal && (code === 0 || code === -1);
  const mins = () => seconds >= 120 ? t('{n} min', { n: Math.floor(seconds / 60) }) : t('{n} s', { n: seconds.toFixed(0) });
  const f = fatal;
  gpuGone = f?.code === 'GPU-LOST';
  // the graphics lost on the way: a reload starts them again (Safari: only a reload), at Play
  if (gpuGone) setPhase('exited', T('Stopped'), () => t('The game was stopped: {what}. Play reloads this page, which starts the graphics again; then press Play once more. Your saves and settings stay.',
    { what: t(f!.what) }) + ' ' + t('Download the log first if you want to keep it: the reload clears it.'), f!.code);
  // the graphics: a restart of the browser, then a newer driver (where to send the log is under it)
  else if (f) setPhase('exited', s.renderer ? T('Stopped') : T('Did not start'), () => t('The game was stopped: {what}. Restart {browser}, then press Play. If it happens again, update the graphics driver.',
    { what: t(f.what), browser: t(browserName()) }), f.code);
  else {
    const base = () => code === 0 ? t('The game closed normally after {time}.', { time: mins() }) : code === -1 ? t('The game was stopped after {time}.', { time: mins() })
      : t('The game stopped with error code {code} after {time}.', { code, time: mins() });
    // what is kept: the game's last profile save, never a claim it did not make
    const sv = saved;
    setPhase('exited', code === 0 ? T('See you') : T('Stopped'), !sv ? base : code === 0 ? () => t('{base} Progress saved in this browser at {when} ({name}).', { base: base(), when: when(sv.at), name: sv.name })
      : () => t('{base} Your progress up to the last save, at {when} ({name}), is kept.', { base: base(), when: when(sv.at), name: sv.name }));
    const seq = subSeq;
    if (!sv) void newestProfile().then(p => {
      if (phase === 'exited' && !session && subSeq === seq)
        setSub(() => `${base()} ${p ? t('Nothing was saved during this game; the last save is from {when} ({name}).', { when: when(p.mtime), name: p.name }) : t('No profile is saved in this browser yet.')}`);
    });
  }
  void saves.refresh();               // new or changed profiles
  vault.gameEnded();                  // and to the keyphrase with them
  showNewer();                        // the note on the log, now that there is one
  void checkNewer();
}

/** the game finished writing a file of the save folder: a profile is the player's progress */
function onSaved(path: string) {
  const p = path.split('/');
  if (p.length !== 3 || p[0] !== PROFILES_DIR || p[2].toLowerCase() !== p[1].toLowerCase()) return;
  saved = { name: p[1], at: Date.now(), count: (saved?.count ?? 0) + 1 };
  log.add(`[app] the game saved profile ${p[1]} (${saved.count} this game)`);
  if (session) toast(t('Progress saved ({name}).', { name: p[1] }), 2500);
  vault.saved();
  if ($<HTMLDialogElement>('#menu').open) $('#menu-save').textContent = t('Last saved at {when} ({name}).', { when: when(saved.at), name: saved.name });
}
/** the newest profile in this browser (listProfiles sorts by time), or null */
async function newestProfile() { return (await listProfiles(SAVE).catch(() => []))[0] ?? null; }
/** a save's time: the clock today, the day too before today */
function when(at: number): string {
  const d = new Date(at), time = d.toLocaleTimeString(dateLocale(), { hour: '2-digit', minute: '2-digit' });
  return d.toDateString() === new Date().toDateString() ? time : `${d.toLocaleDateString(dateLocale(), { day: 'numeric', month: 'short' })}, ${time}`;
}

/** a short message at the bottom of the start page (a status region: read out too); a Msg is drawn again in the
 *  page's language when that changes (a message about the change itself) */
let pageToastTimer: ReturnType<typeof setTimeout> | undefined;
function pageToast(msg: string | Msg) {
  const el = $('#page-toast');
  live(el, typeof msg === 'string' ? () => msg : msg);
  clearTimeout(pageToastTimer);
  pageToastTimer = setTimeout(() => live(el, () => ''), 5000);
}

/** the controls sheet of the start page, in a dialog (F9, Controls, while the game runs), at the page's tab and
 *  button names */
let keySheet: ControlsSheet, helpSheet: ControlsSheet | null = null;
function openHelp() {
  const body = $('#help-body');
  if (!helpSheet) {
    body.append(...[...$('#keys').childNodes].map(n => n.cloneNode(true)));
    helpSheet = controlsSheet(body, 'help');
  }
  helpSheet.show(keySheet.tab);
  helpSheet.family(keySheet.fam);
  $<HTMLDialogElement>('#help').showModal();
}

/** free roam: a friend joined, left, went into a race (who: the friend's name the text starts with); the server
 *  full; four at most, each for 5 s (also in the log).  Friends' lines are only logged for now: with the crowds
 *  on the server they came too often */
const FRIEND_NOTICES = false;
function notice(text: string, who?: string) {
  privacy.name(who, 'player');
  log.add(`[notice] ${text}`);
  if (who && !FRIEND_NOTICES) return;
  const box = $('#notices'), line = document.createElement('div');
  if (who && text.startsWith(who)) { const b = document.createElement('b'); b.textContent = who; line.append(b, text.slice(who.length)); }
  else line.textContent = t(text);                 // the page's own (net/index.ts), English in the log
  placeNotices();
  box.append(line);
  while (box.childElementCount > 4) box.firstElementChild!.remove();
  setTimeout(() => line.remove(), 5000);
}
/** free roam: quick chat (quickchat.ts), a phrase said, from the game; and the lines over the cars in each frame */
function say(json: string) {
  placeNotices();
  quickChat.said(json);
}
function marks(n: number, m: Float32Array, ids: Uint32Array) { quickChat.marks(n, m, ids); }
/** quick chat's line in the log: a speech balloon, the name, the phrase; in the notices' box, for 8 s */
function chatLine(name: string, text: string, me: boolean) {
  const box = $('#notices'), line = document.createElement('div'), b = document.createElement('b');
  line.className = 'chat' + (me ? ' me' : '');
  line.innerHTML = CHAT_ICON;
  b.textContent = name;
  line.append(b, ` ${text}`);
  box.append(line);
  while (box.childElementCount > 4) box.firstElementChild!.remove();
  setTimeout(() => line.remove(), 8000);
}
/** free roam: the race lobby's panel, or the cover over the trip to the race and back (lobby.ts) */
function lobby(json: string) {
  placeLobby();
  lobbyPanel.show(json);
}
function placeLobby() {
  const v = session?.viewRect();
  if (v && v.w && v.h) lobbyPanel.place(v, hudWide);
}
/** The notices' place: on the left, between the game's SMS icon (down to y -80 from the middle) and its radar
 *  (from +64; a GPS icon may sit just above it), lined up with the radar's left rim (x -301), in the game's HUD
 *  units: runtime/widescreen.c shows a 640x480 HUD in the middle of the image, and with hud = wide moves the
 *  radar's side out by up to 109 units (the edge of the image, at most of a centred 16:9 area).  So no size or
 *  shape of the screen puts them over the HUD.  --u: CSS px per HUD unit. */
function placeNotices() {
  const v = session?.viewRect();
  if (!v || !v.w || !v.h) return;
  const u = Math.min(v.w / 640, v.h / 480), e169 = 320 * (16 / 9 * 0.75 - 1);
  const out = hudWide ? Math.min(Math.max(320 * (v.w / v.h * 0.75 - 1), 0), e169) * 109 / e169 : 0;
  const s = $('#notices').style, left = v.x + v.w / 2 - (301 + out) * u, top = v.y + v.h / 2 - 72 * u;
  s.left = `${left}px`;
  s.top = `${top}px`;
  s.setProperty('--u', `${u}px`);
  quickChat.place(left, top, u);               // its picker opens there
}

/** a full server: the game's place in line for one, over the game until it is in (null: not in line); the page menu
 *  offers to stop waiting meanwhile */
let inLine = false;
function netQueue(q: NetQueue | null) {
  inLine = q !== null;
  $<HTMLButtonElement>('#menu [data-act="offline"]').hidden = !inLine;
  const t = $('#queue');
  t.hidden = q === null;
  t.textContent = q ? queueText(q) : '';
}
/** the operator removed the player from playing with others (net/index.ts: a kick or a ban): why, over the game, until
 *  the player plays on (an input REMOVED_MS after it came up) */
const REMOVED_MS = 8000;
let removedTimer: ReturnType<typeof setInterval> | null = null;
function netRemoved(r: NetRemoved) {
  const why = r.msg ? `: ${r.msg}${/[.!?]$/.test(r.msg) ? '' : '.'}` : '.';
  const head = !r.ban ? 'You were removed from playing with others'
    : r.until ? `You can't play with others until ${new Date(r.until).toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' })}`
    : "You can't play with others any more";
  const t = $('#removed');
  t.textContent = `${head}${why} You can keep playing on your own.`;
  t.hidden = false;
  const from = performance.now() + REMOVED_MS;
  if (removedTimer) clearInterval(removedTimer);
  removedTimer = setInterval(() => {
    const last = session?.input?.lastInput?.() ?? null;
    if (last !== null && last > from) hideRemoved();
  }, 500);
}
function hideRemoved() {
  if (removedTimer) { clearInterval(removedTimer); removedTimer = null; }
  $('#removed').hidden = true;
}
/** the away check's message over the game (away.ts), null: none */
function awayLine(text: string | null) {
  const t = $('#away');
  t.hidden = text === null;
  t.textContent = text ?? '';
}
/** the away check's limits; a test page's address may give them in seconds (away=input,hidden,warning) */
function awayLimits(): AwayLimits {
  const [inputS, hiddenS, warnS] = (q.get('away') ?? '').split(',').map(Number);
  const ms = (s: number, d: number) => Number.isFinite(s) && s > 0 ? s * 1000 : d;
  return { inputMs: ms(inputS, AWAY_LIMITS.inputMs), hiddenMs: ms(hiddenS, AWAY_LIMITS.hiddenMs), warnMs: ms(warnS, AWAY_LIMITS.warnMs) };
}
/** the play before our message on the game's phone (playtime.ts); a test page's address may give it in seconds
 *  (playtime=seconds) */
function playTimeLimit(): number {
  const s = Number(q.get('playtime'));
  return Number.isFinite(s) && s > 0 ? s * 1000 : SMS_AFTER_MS;
}
/** the render check's times; a test page's address may give them in seconds of driving (rendercheck=warmup,verdict) */
function renderCheckTimes(): RenderCheckTimes {
  const [warmS, decideS] = (q.get('rendercheck') ?? '').split(',').map(Number);
  const ms = (s: number, d: number) => Number.isFinite(s) && s > 0 ? s * 1000 : d;
  return { warmupMs: ms(warmS, RENDER_CHECK_TIMES.warmupMs), decideMs: ms(decideS, RENDER_CHECK_TIMES.decideMs) };
}

function toast(msg: string, ms: number) {
  const t = $('#toast');
  t.textContent = msg; t.hidden = false;
  clearTimeout((toast as any).timer);
  (toast as any).timer = setTimeout(() => { t.hidden = true; }, ms);
}

/** F12 (photo.ts: photo mode's picture, or where it cannot open): the next frame the renderer draws, as a PNG beside the
 *  saves (Saves on the start page shows it) */
async function screenshot() {
  const r = session?.renderer;
  if (!r?.dump) { toast(t('No screenshot: the picture is not there yet.'), 2500); return; }
  try {
    const png = await r.dump('screenshot');
    if (!png.size) throw new Error('the game ended first');
    const name = await saveScreenshot(SAVE, png);
    log.add(`[app] screenshot ${name}, ${fmtBytes(png.size)}`);
    toast(t('Screenshot saved: Saves on the start page shows it.'), 2500);
  } catch (e) {
    log.add(`[app] the screenshot failed: ${e}`);
    toast(t('The screenshot failed: {error}', { error: e instanceof Error ? e.message : String(e) }), 4000);
  }
}

// ----------------------------------------------------------- page menu --
function openMenu() {
  const m = $<HTMLDialogElement>('#menu');
  if (m.open) { $<HTMLDialogElement>('#playlist').close(); m.close(); return; }                // (its Crews popup too: the menu's close)
  const fsButton = m.querySelector<HTMLButtonElement>('[data-act="fullscreen"]')!;
  fsButton.textContent = isFullscreen() ? t('Leave full screen') : t('Full screen');
  fsButton.hidden = !canFullscreen() && fromHomeScreen();         // from an iPhone's Home Screen: already without bars
  m.querySelector<HTMLButtonElement>('[data-act="fps"]')!.textContent = fpsLabel(fps.mode);
  const solid = m.querySelector<HTMLButtonElement>('[data-act="solid"]')!;
  solid.hidden = solidCars === null || !session?.net?.solid;
  solid.textContent = ghostLabel();
  const crews = m.querySelector<HTMLButtonElement>('[data-act="crews"]')!;
  crews.hidden = roamCrews === null || !session?.net?.crews;
  crews.textContent = crewsLabel();
  m.querySelector<HTMLButtonElement>('[data-act="quit"]')!.textContent = quitLabel();
  showRadio(m);
  const line = $('#menu-save');
  if (saved) line.textContent = t('Last saved at {when} ({name}).', { when: when(saved.at), name: saved.name });
  else {
    line.textContent = t('Nothing saved during this game yet.');
    void newestProfile().then(p => {
      if (!saved && m.open) line.textContent = p ? t('Nothing saved during this game yet. Last save: {when} ({name}).', { when: when(p.mtime), name: p.name }) : t('No profile is saved in this browser yet.');
    });
  }
  $('#stage').classList.add('cursor');
  m.showModal();
}
/** the menu's frame-rate counter and Ghost mode buttons, with what they are at now */
const fpsLabel = (mode: string) => t('Frame-rate counter: {mode}', { mode: t(mode === 'on' ? N('on') : mode === 'detailed' ? N('detailed') : N('off')) });
const ghostLabel = () => solidCars ? t('Ghost mode: off') : t('Ghost mode: on');
const crewsLabel = () => roamCrews?.length ? t('Crews: {n}', { n: roamCrews.length }) : t('Crews: none');
/** The menu's Crews popup (free roam only), over the menu: the Crews row's control, which changes the game's crews at
 *  once.  Done, Esc or a controller's B closes it, back to the menu on its Crews button; an Esc in a crew's palette
 *  closes only the palette. */
function openCrews() {
  const pop = $<HTMLDialogElement>('#crews-pop');
  if (roamCrews === null || !session?.net?.crews || pop.open) return;
  drawPopCrews();
  pop.showModal();
  ($('#crews-pop-box').querySelector<HTMLElement>('input:enabled') ?? $('#crews-pop-box').querySelector<HTMLElement>('.crew-dot'))?.focus();
  void askPopCounts();
}
/** The popup's counts, asked of the server once (the page does not ask while a game runs), after ms: a Join reaches
 *  the server's hub with the game's next HELLO.  As the server counts them: the player too, once in free roam in that
 *  crew.  Shown while the popup is open; the newest question's only. */
async function askPopCounts(ms = 0) {
  const n = ++popAsk;
  if (ms) await new Promise(r => setTimeout(r, ms));
  if (n !== popAsk || offline || !roamCrews?.length) return;
  const counts = await askCrews(roamCrews.map(c => roomHash(c.name)), 5000);
  if (!counts || n !== popAsk) return;
  popCounts = counts;
  if ($<HTMLDialogElement>('#crews-pop').open) showCrewCounts($('#crews-pop-box'), counts);
}
/** the popup's control, drawn again after each change; focus: where the keyboard (or controller) goes then, if it was
 *  in there */
function drawPopCrews(focus?: (root: HTMLElement) => Element | null | undefined) {
  const box = $('#crews-pop-box'), had = box.contains(document.activeElement);
  box.replaceChildren();
  crewControl({ list: roamCrews ?? [], id: 'crews-pop-input', labelId: 'crews-pop-title', placeholder: N('Crew name'), counts: popCounts, draft: crewDraft,
    onDraft: v => { crewDraft = v; }, change: (list, f) => void menuCrews(list, f) }, box).setAttribute('aria-labelledby', 'crews-pop-title');
  if (had && focus) ((focus(box) ?? box.querySelector('input:enabled')) as HTMLElement | null)?.focus({ focusVisible: true } as FocusOptions);
}
/** a Leave, Join or colour in the popup: the game takes the crews and their colours at once (its HELLO again to the
 *  hub), and settings.ini keeps them for the next game, as Settings, Online, Crews would */
async function menuCrews(list: Crew[], focus: (root: HTMLElement) => Element | null | undefined) {
  if (roamCrews === null || !session?.net?.crews) return;
  roamCrews = list;
  session.net.crews(list.map(c => roomHash(c.name)), rgbWords(list));
  drawPopCrews(focus);
  void askPopCounts(1500);
  $('#menu [data-act="crews"]').textContent = crewsLabel();
  let text = putKey(settingsText ?? TEMPLATE, 'roam_crew', list.length ? crewsValue(list.map(c => c.name)) : null).text;
  text = putKey(text, 'roam_crew_colours', list.length ? rgbsValue(list) : null).text;
  try { await writeFile(SAVE, 'settings.ini', text); settingsText = text; void settings.refresh(); }
  catch (e) { log.add(`[app] settings.ini not written: ${e}`); }
  log.add(`[app] crews: ${list.length ? list.map(c => `${c.name} (${c.rgb})`).join(', ') : 'none'} (the page menu)`);
}
function showRadio(m: HTMLElement) {
  const button = m.querySelector<HTMLButtonElement>('[data-act="radio"]')!, on = radio?.on ?? false;
  m.querySelector<HTMLElement>('.menu-radio')!.hidden = !radioPort() || !canPickMusic();    // a game without the radio's words (an older build) has no Radio row
  button.classList.toggle('selected', on);
  button.setAttribute('aria-pressed', String(on));
  $('#radio-label').textContent = t('Radio: {mode}', { mode: on ? t(N('on')) : t(N('off')) }) + (radio?.folder ? ` · ${radio.folder}` : '');
}
async function radioClick(act: 'radio' | 'radio-folder') {
  const r = theRadio();
  try { await (act === 'radio-folder' ? r.chooseFolder(pickMusic) : r.on ? r.turnOff() : r.turnOn(pickMusic)); } catch (e) { log.add(`[radio] ${act}: ${e}`); }
}
const MODE_WORDS: Record<Mode, string> = { menu: N('Menu'), racing: N('Race'), all: N('All'), off: N('Off') };
/** The Playlist window lists the folder's tracks with their modes; drawn once for a folder, one change listener for all rows. */
let playlistDrawn: readonly string[] | null = null;
function openPlaylist() {
  const body = $('#playlist-body'), paths = radio?.trackPaths ?? [], make = (tag: string, props: object) => Object.assign(document.createElement(tag), props);
  if (playlistDrawn !== paths) {
    playlistDrawn = paths;
    const rows = document.createDocumentFragment();
    let folder = '';
    paths.forEach((name, i) => {
      const at = name.lastIndexOf('/'), dir = name.slice(0, Math.max(0, at));
      if (dir !== folder) rows.append(make('div', { className: 'track-folder', textContent: folder = dir }));
      const seg = make('div', { className: 'seg' });
      seg.setAttribute('role', 'radiogroup'); seg.setAttribute('aria-label', name);
      for (const m of MODES) {
        const opt = make('label', { className: 'opt' }), input = make('input', { type: 'radio', name: `mode${i}`, value: m, checked: radio?.mode(name) === m });
        input.dataset.track = String(i);
        opt.append(input, t(MODE_WORDS[m]));
        seg.append(opt);
      }
      const row = make('div', { className: 'track' });
      row.append(make('span', { textContent: name.slice(at + 1), title: name }), seg);
      rows.append(row);
    });
    body.replaceChildren(rows);
    if (!paths.length) body.textContent = t('No music yet: pick a folder first.');
  }
  $('#playlist-shuffle').hidden = !paths.length;
  $<HTMLInputElement>('#playlist-shuffle input').checked = radio?.shuffle ?? false;
  $<HTMLDialogElement>('#playlist').showModal();
}
const setShuffle = (e: Event) => radio?.setShuffle((e.target as HTMLInputElement).checked);
function setTrackMode(e: Event) {
  const r = e.target as HTMLInputElement, name = playlistDrawn?.[Number(r.dataset.track)];
  if (name !== undefined) radio?.setMode(name, r.value as Mode);
}
async function menuAction(act: string) {
  const m = $<HTMLDialogElement>('#menu');
  switch (act) {
    case 'resume': m.close(); break;
    case 'fullscreen':
      m.close();
      toggleFullscreen();
      break;
    case 'fps': fps.cycle(); m.querySelector<HTMLButtonElement>('[data-act="fps"]')!.textContent = fpsLabel(fps.mode); break;
    case 'solid': {
      if (solidCars === null || !session?.net?.solid) break;
      solidCars = !solidCars;
      session.net.solid(solidCars);
      m.querySelector<HTMLButtonElement>('[data-act="solid"]')!.textContent = ghostLabel();
      // and in settings.ini for the next game, as Settings, Online, Ghost mode would
      const text = putKey(settingsText ?? TEMPLATE, 'roam_collide', solidCars ? 'on' : 'off').text;
      try { await writeFile(SAVE, 'settings.ini', text); settingsText = text; void settings.refresh(); }
      catch (e) { log.add(`[app] settings.ini not written: ${e}`); }
      log.add(`[app] solid cars ${solidCars ? 'on' : 'off'} (the page menu)`);
      break;
    }
    case 'crews': openCrews(); break;
    case 'radio-folder': case 'radio': await radioClick(act); showRadio(m); break;
    case 'playlist': openPlaylist(); break;
    case 'help': m.close(); openHelp(); break;
    case 'offline':
      m.close();
      if (!inLine || !session?.net?.offline) break;
      session.net.offline();
      toast(t('Playing offline: you left the line. Free roam and Online stay off until the next game.'), 5000);
      break;
    case 'log': downloadLog(); break;
    case 'quit': {
      m.close();
      const s = session;
      if (!s) break;
      const stage = quitStage(s);
      if (stage === 0) {
        if (s.requestQuit()) { quitAt = { s, t: performance.now(), forced: false }; toast(t('Quitting: the game saves and closes.'), 5000); break; }
      } else if (stage === 1) {
        // the game's own second-request rule: open save files are finished, then it exits
        if (await confirmBox(t('Close the game now?'), t('The game has not closed yet (a loading screen ignores Quit). Closing now finishes any save in progress first.'), t('Close now'), true, 'quit') && session === s) {
          if (s.requestQuit()) { quitAt = { s, t: performance.now(), forced: true }; toast(t('Closing the game.'), 4000); break; }
        } else break;
      }
      // the game cannot be asked (any more), or it did not close when asked twice
      if (await confirmBox(t('Stop the game now?'), s.canQuit()
          ? t('The game did not close when asked. Stopping it ends it at once: progress since it last saved is lost.')
          : t('The game cannot be asked to quit, so it is stopped at once. Progress since it last saved is lost.'), t('Stop the game'), true, 'quit') && session === s) s.terminate();
      break;
    }
  }
}

// Quit game: first the game's own quit (WM_CLOSE, as Cmd-Q on the Mac); if it
// is still running, a second request makes it finish open saves and exit; only
// if even that has not ended it once its longest possible run is over does the
// page offer to stop it.  That run (runtime/web/input_web.c force_exit): up to
// 5 s for open save files (lifecycle_drain_writes), then up to 2 s to freeze
// the guest threads; STOP_AFTER_MS leaves a margin above those 7 s, so the page
// never offers to cut a save in half that the game is still finishing.
const FORCED_CLOSE_MAX_MS = 5000 + 2000;
const STOP_AFTER_MS = FORCED_CLOSE_MAX_MS + 3000;
let quitAt: { s: GameSession; t: number; forced: boolean } | null = null;
function quitStage(s: GameSession): 0 | 1 | 2 {
  if (!s.canQuit()) return 2;
  if (!quitAt || quitAt.s !== s) return 0;
  return quitAt.forced && performance.now() - quitAt.t > STOP_AFTER_MS ? 2 : 1;
}
function quitLabel(): string {
  if (!session) return t('Quit game');
  return [t('Quit game'), t('Close the game now'), t('Stop the game')][quitStage(session)];
}

// ------------------------------------------------------------- helpers --
function confirmBox(title: string, body: Node | string, ok: string, danger = false, about = '', cancel = t('Cancel')): Promise<boolean> {
  return choiceBox(title, body, ok, cancel, danger, about).then(v => v === 'ok');
}
/** the same, with which way it closed: 'ok', 'cancel' (the cancel button, none when cancel is ''), '' (Esc) */
function choiceBox(title: string, body: Node | string, ok: string, cancel: string, danger = false, about = ''): Promise<string> {
  const d = $<HTMLDialogElement>('#confirm');
  d.dataset.about = about;            // 'quit': closed when the game ends (endStage)
  $('#confirm-title').textContent = title;
  const b = $('#confirm-body'); b.replaceChildren(typeof body === 'string' ? Object.assign(document.createElement('p'), { textContent: body }) : body);
  const okb = $<HTMLButtonElement>('#confirm-ok'); okb.textContent = ok; okb.className = danger ? 'btn danger' : 'btn selected';
  const cb = $('#confirm-cancel'); cb.textContent = cancel; cb.hidden = !cancel;     // '': the one button
  d.returnValue = '';
  d.showModal();
  return new Promise(res => d.addEventListener('close', () => res(d.returnValue), { once: true }));
}
// A frozen game's log: players download it before the stall report's 15 s (2026-10-24: 13 s, no report), so the
// game is asked first: how long ago its last frame was and, after 3 s or more, what every thread is doing
// (runtime/watchdog.c).  No answer in 2 s, or a game that cannot be asked: the log as it is.
async function downloadLog() {
  if (session?.logReport()) await log.waitFor('[watchdog] log download ready', 2000);
  // the last game's copy first, when that game did not get to its end (lastgame.ts)
  // as it may leave the browser (privacy.ts): no names, no addresses; a copy from a page before that scrubbed nothing
  const kept = lastGame ? (lastGame.scrubbed ? lastGame.log : privacy.scrub(lastGame.log)) : '';
  const prev = lastGame ? `=== the last game's log, as the page kept it while it ran ===\n${kept}=== this page's log ===\n` : '';
  downloadBlob(`${SCRUB_NOTE}\n${prev}${privacy.scrub(log.text())}`, `nfsu2-${stamp()}.log`, 'text/plain');
}
/** what the running game holds now, for each copy of its log (lastgame.ts): what fills a phone's memory as it plays */
function gameHeld(): string {
  const parts: string[] = [];
  const tx = session?.renderer?.textures?.(), MB = (b: number) => (b / 1048576).toFixed(0);
  if (tx) parts.push(`textures ${MB(tx.bytes)} MB${texSplit(tx.unpacked, tx.astc)}`);
  const st = session?.status?.read(), cars = st?.get('roam_cars') ?? 0, friends = st?.get('roam_friends') ?? 0;
  if (cars || friends) parts.push(`${cars} friends' cars in the game, ${friends} friends connected`);
  return parts.join(', ');
}

// --------------------------------------------------------- game module --
// A failed download (Wi-Fi drop, the server restarting, the module not built
// yet) is its own check, tried again with back-off (2, 4, 8 ... 30 s), at the
// next answer from the server, or with Retry.
let moduleLoading = false, moduleFails = 0, moduleTimer: ReturnType<typeof setTimeout> | undefined;
async function loadModule() {
  clearTimeout(moduleTimer);
  if (moduleLoading || loaded) return;
  moduleLoading = true;
  setCheck(moduleFails ? check('module', N('Game module'), 'pending', N('Loading again (attempt {n}).'), undefined, { n: moduleFails + 1 })
    : check('module', N('Game module'), 'pending', N('Loading.')));
  try {
    loaded = await loadGame((got, total) => {
      setProgress(total ? got / total : 0);
      if (phase === 'boot') setSub(T('Loading the game: {got} of {total}.', { got: fmtBytes(got), total: fmtBytes(total) }));
    });
    setProgress(1);
    log.add(`[app] game module ${loaded.info.rev}: ${fmtBytes(loaded.info.size)} in ${loaded.ms.toFixed(0)} ms, ${loaded.transferred} bytes transferred${moduleFails ? `, after ${moduleFails} failed attempts` : ''}`);
    moduleFails = 0;
    moduleLoading = false;
    setCheck(check('module', N('Game module {rev}'), 'ok', N('{size}, ready.'), undefined, { rev: loaded.info.rev, size: fmtBytes(loaded.info.size) }));
    keepForOffline();
    afterUpdate();
    void checkNewer();
  } catch (e) {
    moduleFails++;
    const wait = Math.min(30, 2 ** moduleFails);
    log.add(`[app] the game module did not load (attempt ${moduleFails}): ${e}; next try in ${wait} s`);
    moduleLoading = false;
    setProgress(0);
    setCheck(check('module', N('Game module'), 'fail', N('The game did not load ({error}). Trying again in {n} s, or press Retry.'), 'MODULE-LOAD', { error: String(e), n: wait }));
    moduleTimer = setTimeout(() => void loadModule(), wait * 1000);
  }
}

// ------------------------------------------------------- newer version --
// The module stays loaded while the page is open, through every game it plays,
// so a page left open plays the version it loaded first even once the server
// has a newer one (a player's log of 2026-10-20: one page, open from 07:50 to
// 11:10 UTC, ran four games on it).  A page can also start from offline mode's
// copies (sw.ts) of the version before: a deploy restarts the server, its
// players reload, and the server is away for a moment.  So the page asks the
// server which version it has: once the module has loaded, when the server
// answers again, when the tab is shown, after each game, at Play, every 5
// minutes while the tab is visible (during a game too), and when the game's
// link to the server comes back after losing it (a restart).  When that is not
// the loaded one:
//   nothing here a reload would lose (no game ran in this page, no question
//     open, no field being typed in, no game data downloading): the page
//     reloads by itself, which brings the page and the module of the new
//     version together, and says so;
//   a game runs: a word over it, once; the start page offers it when it ends;
//   else the start page offers it (Reload), and Play reloads too: after a game
//     with a word on its log, which the reload clears (found only at Play: the
//     offer comes first, and the next Play reloads).
// sessionStorage names the version a reload was for: a reload that still came
// back without it offers it, and is not repeated by itself.  A test page (the
// address's test settings) only offers it: its tool drives it.
const NEWER_EVERY_MS = 5 * 60_000, UPDATE_KEY = 'nfsu2-update';
let newer: string | null = null;
/** the version a reload of this tab was for and did not bring */
let tried: string | null = null;
/** this page came from a reload for the version it has (the start page says so until Play) */
let updated = false;
let asking = false;
const testPage = () => TEST_PARAMS.some(k => q.has(k));
/** Play reloads for the newer version (not for one a reload did not bring, nor offline: the copies would answer) */
const byPlay = () => !!newer && newer !== tried && !offline && !testPage();

async function checkNewer() {
  if (!loaded || newer || offline || asking) return;
  asking = true;
  const rev = await serverRev(5000);
  asking = false;
  if (!rev || rev === loaded.info.rev || newer) return;
  newer = rev; updated = false;
  const playing = !!session || starting;
  if (!playing && calm() && await reloadFor(rev, 'nothing on the page to lose', true)) return;
  log.add(`[app] the server has game ${rev} now, this page ${loaded.info.rev}: ${playing ? 'said over the game, offered when it ends' : 'Reload offered'}`);
  if (playing) toast(t('A new version of the game is on the server. Quit when it suits you: the start page offers it, and your saves stay.'), 10000);
  showNewer();
}
/** nothing on the start page a reload would lose, and no tool driving it */
function calm(): boolean {
  const a = document.activeElement;
  return phase !== 'exited' && newer !== tried && !testPage() && !download.running && !document.querySelector('dialog[open]') &&
    !a?.matches('textarea, input:not([type=radio]):not([type=checkbox])');
}
/** the game's link to the server came back after losing it (a server restart, as for a deploy): asked within 30 s,
 *  spread out, so that a restarted server's players do not all ask at once */
function netBack() { setTimeout(() => void checkNewer(), Math.random() * 30_000); }
/** Reload for the server's version rev.  sessionStorage names it (afterUpdate); by itself only then (false: not
 *  reloaded), at Play either way. */
async function reloadFor(rev: string, why: string, byItself: boolean): Promise<boolean> {
  let marked = false;
  try { sessionStorage.setItem(UPDATE_KEY, rev); marked = true; } catch { /* no storage here */ }
  if (byItself && !marked) return false;
  log.add(`[app] the server has game ${rev} now, this page ${loaded!.info.rev}: reloading (${why})`);
  await reload();
  return true;
}
/** after a reload for a new version: whether it brought it */
function afterUpdate() {
  let want: string | null = null;
  try { want = sessionStorage.getItem(UPDATE_KEY); } catch { /* no storage here */ }
  if (!want || !loaded) return;
  if (want === loaded.info.rev) {
    try { sessionStorage.removeItem(UPDATE_KEY); } catch { /* no storage here */ }
    updated = true;
    log.add(`[app] reloaded for game ${want}: this page has it`);
  } else {
    tried = want;
    log.add(`[app] reloaded for game ${want}, but this page has ${loaded.info.rev}: offered, not reloaded for again`);
  }
  showNewer();
}
/** the offer (once a game has run here, with a word on its log: only in this page, a reload clears it), or that
 *  this page is the new version */
function showNewer() {
  const line = $('#newer'), byP = byPlay(), log = !!lastExit, n = newer, u = updated;
  live(line, () => n ? (byP ? t('A new version of the game is on the server: Play reloads this page with it. Your saves and settings stay.')
    : t('A new version of the game is on the server. Reload this page to play it: your saves and settings stay.')) +
    (log ? ' ' + t('Download the log first if you want to keep it: the reload clears it.') : '')
    : u ? t('This page reloaded for the new version of the game. Your saves and settings stayed.') : '');
  line.hidden = !newer && !updated;
  $('#reload').hidden = !newer;
}
async function reload() {
  await saves.settled();              // a rename or restore the Saves section is writing ends first
  await swOnline();
  location.reload();
}
/** A reload offline mode's copies do not answer (otherBuild): the service worker goes first.  The page registers it
 *  again once the module has loaded, and its install takes new copies from the server. */
async function reloadFromServer() {
  await saves.settled();
  try { await (await navigator.serviceWorker?.getRegistration())?.unregister(); } catch { /* none, or not here */ }
  location.reload();
}
/** Offline mode's copies answer first for a while after the server did not (sw.ts); it answers now, so the start
 *  after this reload is the server's.  An older service worker does not answer: 0.5 s at most. */
function swOnline(): Promise<void> {
  const sw = navigator.serviceWorker?.controller;
  if (!sw) return Promise.resolve();
  return new Promise(res => {
    const ch = new MessageChannel();
    ch.port1.onmessage = () => res();
    sw.postMessage('online', [ch.port2]);
    setTimeout(res, 500);
  });
}

// ----------------------------------------------------------------- boot --
/** the start page says when the address carries test settings: in effect (a development server) or ignored */
function showOverrides(given: string[], dev: boolean) {
  const o = $('#override');
  o.hidden = !given.length;
  if (!given.length) return;
  if (!dev) {
    live(o, T('This page\'s address carries test settings ({names}). They work only on a development server and are ignored here: the game runs with your own settings.', { names: given.join(', ') }));
    return;
  }
  const env = Object.entries(ENVIRONMENT).map(([k, v]) => `${k}=${v}`);
  live(o, () => t('Test settings from this page\'s address (development server): save folder "{folder}"', { folder: SAVE }) +
    (env.length ? t('; {vars} (wins over Settings)', { vars: env.join(', ') }) : '') +
    ['fresh', 'reg', 'script', 'artifacts', 'dumpscale', 'away', 'playtime'].filter(k => q.has(k)).map(k => `; ${k}`).join('') + '.');
}

export async function boot() {
  // the address's test settings: asked about only when there are some (a player's page asks nothing)
  const asked = TEST_PARAMS.some(k => addr.has(k));
  const dev = asked && await devServer();
  const given = takeParams(dev);
  instance = claimInstance(`nfsu2-saves:${SAVE}`);
  // the last game of this save folder that did not get to its end (Safari stopped the page, say): its log is offered
  keeper = new LastGameKeeper(SAVE, log, logLine, text => privacy.scrub(text));
  lastGame = lastGameToOffer(SAVE);
  if (lastGame) {
    log.keep(lastGameLine(lastGame));
    // a reload or a closed tab is the player's own doing: the log stays at hand (a frozen game, reloaded), unasked for
    const end = lastGameEnd(lastGame), why = lastGame.ended?.why;
    live($('#lastgame'), end === 'stopped' ? T('Your last game stopped without warning. Phones do this when they run low on memory.')
      : end === 'error' ? T('Your last game stopped with an error.')
      : T('Reloaded because something went wrong? Download log still has what happened in your last game.'),
      end === 'error' && why !== 'exit' ? why : undefined);
    $('#log-btn').hidden = $('#lastgame').hidden = false;
    $('#report').hidden = end === 'left';
  }
  fps = new FpsOverlay($('#fps'));
  lobbyPanel = new LobbyPanel($('#lobby'), $('#lobby-cover'), logLine);
  // made here once: a closure made per game (in startGame) would keep that game's scope reachable
  fps.extra = () => session?.stats() ?? {};
  // the language this browser showed last, at once (settings.ini says which it is: applyLanguage, once it is read)
  void setLanguage(lastLanguage(), logLine);
  settings = new SettingsPanel($('#settings'), {
    // another language or texture pack counts other files as the game data (its pack); a keyphrase sends the file
    saveDir: SAVE, environment: ENVIRONMENT, defaults: DEVICE, onSaved: text => {
      const was = language() + '/' + texpack(); settingsText = text;
      if (language() + '/' + texpack() !== was) void saves.refresh();
      vault?.changed(); players?.now();
      applyLanguage();
    },
    onLoaded: applyLanguage,
    // the tab that owns the saves is the one whose Play reads the file: another tab only looks
    readOnly: () => !instance.owner, say: pageToast, confirm: confirmBox,
  }, $('#online'));
  // the language picker at the top: the same setting as Settings, Game, Language
  const pick = $<HTMLSelectElement>('#lang-pick');
  pick.replaceChildren(...LANGUAGES.map(l => Object.assign(document.createElement('option'), { value: l, textContent: NATIVE[l], lang: LOCALE[l] })));
  pick.onchange = () => void settings.set('language', pick.value);
  // who plays on this server now (Online): only while no game runs here; with free roam on in a room, that room too,
  // and in crews (free roam on or not: the row shows them either way), how many are in each (on the Crews row's lines)
  players = watchPlayers((line, crews) => { settings.setStatus(line); settings.setCrewCounts(crews); }, () => !session && !starting, setReach,
    () => { const e = resolved().env; return e.NFSU2_ROAM === '1' ? e.NFSU2_ROAM_ROOM ?? '' : ''; },
    () => crewList(resolved().env.NFSU2_ROAM_CREW ?? '').map(roomHash));
  window.addEventListener('online', () => players?.now());
  window.addEventListener('resize', placeNotices);
  window.addEventListener('resize', placeLobby);
  download = new Predownload(s => saves.showData(s), logLine, () => language(), () => texpack(), () => voices());
  saves = new SavesPanel($('#saves'), $('#game-data'), {
    saveDir: SAVE, busy: () => !!session || starting || !instance.owner, confirm: confirmBox,
    onRestored: () => { void loadSettings(); void settings.refresh(); }, download, onChanged: () => vault.changed(),
  });
  // keyphrase saves (on a server that keeps them): synced by the tab that owns the saves, once its settings are loaded
  vault = new VaultPanel($('#vault'), {
    saveDir: SAVE, owner: () => instance.owner, playing: () => !!session, busy: () => !!session || starting || !instance.owner,
    confirm: confirmBox, choose: (title, body, ok, cancel) => choiceBox(title, body, ok, cancel),
    onRestored: () => { void loadSettings(); void settings.refresh(); void saves.refresh(); }, log: logLine,
  });
  $<HTMLButtonElement>('#play').onclick = () => void play();
  $<HTMLButtonElement>('#retry').onclick = () => void loadModule();
  $<HTMLButtonElement>('#reload').onclick = () => void reload();
  $<HTMLButtonElement>('#log-btn').onclick = downloadLog;
  $('#menu').addEventListener('click', e => { const a = (e.target as HTMLElement).closest<HTMLButtonElement>('[data-act]'); if (a) void menuAction(a.dataset.act!); });
  $('#playlist-body').addEventListener('change', setTrackMode);
  $('#playlist-shuffle').addEventListener('change', setShuffle);
  $('#menu').addEventListener('close', () => {
    const pop = $<HTMLDialogElement>('#crews-pop'); if (pop.open) pop.close();      // the menu's Crews go with it
    $('#stage').classList.remove('cursor'); $('#stage').querySelector('canvas')?.focus();
  });
  // the menu's Crews popup: Done closes it; back on the menu's Crews button, with its count as it is now
  $('#crews-pop').addEventListener('click', e => { if ((e.target as HTMLElement).closest('[data-act="crews-done"]')) $<HTMLDialogElement>('#crews-pop').close(); });
  $('#crews-pop').addEventListener('close', () => {
    const b = $<HTMLButtonElement>('#menu [data-act="crews"]');
    b.textContent = crewsLabel();
    if ($<HTMLDialogElement>('#menu').open) b.focus();
  });

  // F8 / F9 belong to the page while the game runs (the game binds neither,
  // Chrome reserves neither); caught first, so the game never sees them.  F12
  // too: photo mode in the world, its picture while it is on, else the page's
  // screenshot (not the game's, whose copy of the frame comes out black in
  // this port, nor the browser's developer tools).
  window.addEventListener('keydown', e => {
    if (session && (e.code === 'F8' || e.code === 'F9' || e.code === 'F12')) {
      e.preventDefault(); e.stopImmediatePropagation();
      if (e.repeat) return;
      if (e.code === 'F8') toast(fpsLabel(fps.cycle()), 1500); else if (e.code === 'F9') openMenu(); else photo.f12();
      return;
    }
    // Enter plays only when nothing else has the focus (or Play itself does): a
    // focused link, button or field keeps its own Enter
    const t = e.target as Element | null;
    const onPage = !t || t === document.body || t === document.documentElement || t.id === 'play';
    if (!session && e.code === 'Enter' && onPage && !document.querySelector('dialog[open]') && !$<HTMLButtonElement>('#play').disabled) {
      e.preventDefault(); void play();
    }
  }, { capture: true });
  window.addEventListener('beforeunload', e => { if (session) { e.preventDefault(); e.returnValue = ''; } });
  // a game running when the page goes away (closed, reloaded): said so in its copy (Safari stopping it says nothing)
  window.addEventListener('pagehide', () => { if (session) keeper.end(-2, 'page closed'); });
  // a hidden page is what a phone stops first: the copy then, and how long it was away in the log
  document.addEventListener('visibilitychange', () => {
    if (!session) return;
    if (document.hidden) hiddenAt = performance.now();
    else if (hiddenAt) { log.add(`[app] the page was hidden for ${((performance.now() - hiddenAt) / 1000).toFixed(1)} s (t=${((performance.now() - session.started) / 1000).toFixed(0)} s into the game)`); hiddenAt = 0; }
    keeper.hidden(document.hidden);
  });
  document.addEventListener('fullscreenchange', () => { if (!isFullscreen() && session) toast(t('Full screen left. {menu} has it.', { menu: cap(menuName()) }), 3000); });
  noPinchZoom();
  // the menu's button on the game: on a touch screen from the start, elsewhere from the first touch
  markTouch();
  keySheet = controlsSheet($('#keys'), 'keys', { follow: true });
  // what the page drew itself, again in the language picked (the markup and the live texts redraw themselves)
  onLanguage(() => {
    renderProblems(); evaluate(); settings.render(); void saves.refresh(); vault.refresh(); keySheet.relabel(); lobbyPanel.redraw(); touch.relabel();
    helpSheet = null; $('#help-body').replaceChildren();
    syncPicker();
  });
  const stageMenu = $<HTMLButtonElement>('#stage-menu');
  stageMenu.onpointerdown = e => e.preventDefault();     // the canvas keeps the focus (and with it the keyboard)
  stageMenu.onclick = () => { if (session) openMenu(); };
  $('#stage').addEventListener('pointerdown', e => { if (e.pointerType === 'touch') stageMenu.hidden = false; }, { capture: true });
  quickChat = new QuickChat($('#stage'), { line: chatLine, lobby: (name, text) => lobbyPanel.say(name, text), place: placeNotices, log: logLine });
  touch = new TouchControls($('#stage'), { toast, log: logLine, chat: () => quickChat.toggle() });
  textField = new TextField($('#stage'), { log: logLine });
  pauseHint = new PauseHint($('#stage'), {
    open: openMenu, log: logLine,
    roam: () => solidCars !== null && !!session?.net?.solid,
    fullscreen: () => canFullscreen() || !fromHomeScreen(),
  });
  driftPanel = new DriftPanel($('#stage'), { toast, wide: () => hudWide, log: logLine });
  photo = new PhotoMode($('#stage'), { shoot: screenshot, screenshot: () => void screenshot(), log: logLine });
  document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'visible') { void checkNewer(); vault.shown(); } });
  setInterval(() => { if (document.visibilityState === 'visible') void checkNewer(); }, NEWER_EVERY_MS);

  setCheck(checkBrowser());
  setCheck(checkIsolation());
  setCheck(check('gpu', N('Graphics (WebGPU)'), 'pending', N('Checking.')));
  setCheck(check('storage', N('Browser storage'), 'pending', N('Checking.')));
  setCheck(check('module', N('Game module'), 'pending', N('Loading.')));
  setPhase('boot', T('Loading'), T('Loading the game.'));
  setProgress(0);

  showOverrides(given, dev);
  if (given.length) log.add(dev ? `[app] test settings from the page address: ${given.join(', ')}` : `[app] the page address's test settings (${given.join(', ')}) are ignored: not a development server`);
  if (q.has('fresh') && SAVE !== 'save') { await removeSaveDir(SAVE); forgetLink(SAVE); log.add(`[app] fresh save folder ${SAVE}`); }
  // the settings and saves show at once (read only until this tab owns the saves), and again once it does
  void settings.refresh();
  void saves.refresh();
  void vault.start();
  void instance.whenOwner.then(async () => { await loadSettings(); evaluate(); void settings.refresh(); void saves.refresh(); vault.ownerNow(); syncPicker(); });
  void checkGpu(lastGame?.ended?.why === 'GPU-LOST').then(c => { setCheck(c); if (c.code === 'GPU-ADAPTER-AFTER-LOST') recheckGpu(); });
  // Firefox's answer to its storage prompt comes later: the check again then
  void checkStorage().then(({ answer, ...c }) => { setCheck(c); void answer?.then(setCheck); });
  refreshMeasured = measureRefresh(60).then(r => { displayHz = r.hz; log.add(`[app] display refresh ${r.raw.toFixed(1)} Hz -> ${r.hz}`); });

  if (crossOriginIsolated) await loadModule();
  else setCheck(check('module', N('Game module'), 'fail', N('Not loaded: the page is not isolated (see above).'), 'MODULE-NOT-ISOLATED'));
  evaluate();
  (window as any).__nfsu2 = {
    get phase() { return phase; }, get lastExit() { return lastExit; }, get fps() { return { game: fps.gameFps, page: fps.pageFps, source: $('#fps').dataset.source }; },
    get session() { return session; }, log, save: SAVE, download: () => download.status, offline: () => offline, displayHz: () => displayHz, retryModule: () => loadModule(),
    newer: () => newer, tried: () => tried, touch: () => touch.state(), textField: () => textField.state(), renderCheck: () => renderCheck?.state() ?? null,
    quickChat: () => quickChat.state(), quickChatStick: (x: number, y: number) => quickChat.stick(x, y),
    env: () => resolved().env, settingsText: () => settingsText, checks: () => [...checks.values()],
    fpsMode: (m?: FpsMode) => { if (m && FPS_MODES.includes(m)) fps.set(m); return fps.mode; },
    quitTimes: { forcedCloseMaxMs: FORCED_CLOSE_MAX_MS, stopAfterMs: STOP_AFTER_MS },
  };
}
