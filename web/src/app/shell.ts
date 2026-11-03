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
import { browserName, check, checkBrowser, checkGpu, checkIsolation, checkStorage, fmtBytes, NO_JIT, noJit, type Check } from './checks';
import { codeOf, dateLocale, language as pageLanguage, lastLanguage, LANGUAGES, listOf, live, LOCALE, N, NATIVE, onLanguage, setLanguage, t, T, tn, tvars, type Msg } from './i18n';
import { loadGame, serverRev, type Loaded } from './loader';
import { claimInstance } from './instance';
import { LogRing, downloadBlob, stamp } from './logs';
import { LastGameKeeper, lastGameEnd, lastGameLine, lastGameToOffer, type LastGame } from './lastgame';
import { androidMajor, browserBrief, deviceBrief } from './devicelog';
import { canFullscreen, enterFullscreen, exitFullscreen, fromHomeScreen, homeScreenInstead, isFullscreen, keepScreenOn, measureRefresh, settledSize, trackCanvasSize } from './display';
import { deviceDefaults, isPhone, markTouch, noPinchZoom, padSeen, phoneScreen, touchFirst } from './device';
import { phoneSeed, phoneUpgrade } from './phonegraphics';
import { markPad, padPress, pageDialog } from './padnav';
import { controlsSheet, type ControlsSheet, type Tab } from './controls';
import { TouchControls } from './touch';
import { TextField } from './textentry';
import { PauseHint } from './pausehint';
import { DriftPanel } from './drift';
import { PhotoMode, photoMark } from './photo';
import type { PadPage } from '../input/gamepad';
import type { NetQueue, NetRaces, NetRemoved, NetRename } from '../net';
import { blockList, checkSetting, HOST_FIXED, pageOnly, resolveEnv, settingsUpgrade, TEMPLATE, type Resolved } from './settings';
import { exists, filesOf, listProfiles, PROFILES_DIR, readText, removeSaveDir, settleStaged, writeFile } from './savefs';
import { coverOf, newProfileName, turnOnAutosave } from './profile';
import { crewControl, putKey, SettingsPanel, showCrewCounts } from './settings-ui';
import { askCrews, PlayersPanel, queueText, roomHash, roomLine, roomText, serverAnswers, watchPlayers, type PlayerCounts, type Reach } from './players';
import { panelTabs } from './tabs';
import { CREW_MAX, crewList, crewsOf, crewsValue, freeColour, rgbsValue, rgbWords, type Crew } from './crews';
import { checkInvite, inviteLink, readInvite, sameName, type Invite } from './invite';
import { privacy, SCRUB_NOTE } from './privacy';
import { oneWriter, SavesPanel, snapshotFile, snapshotSaves, type FileIn } from './saves-ui';
import { zip } from './zip';
import { saveScreenshot } from './shots';
import { forgetLink, VaultPanel } from './vault-ui';
import { Radio, MODES, radioWasOn, type Mode } from '../radio';
import { canPickMusic, pickMusic } from '../radio-folder';
import { RadioLink } from './radio-link';
import { FpsOverlay, FPS_MODES, type FpsMode } from './overlay';
import { LobbyPanel } from './lobby';
import { ICON as CHAT_ICON, QuickChat } from './quickchat';
import { groupsOf, phrasesOf } from './phrases';
import { FriendPrompt, FriendsApi, FriendsPanel } from './friends';
import { ChatDialog, FriendMessages, MSG_ICON, MSG_KEY_LABEL, MessageStore } from './messages';
import { GameSession } from './game';
import { AwayCheck, AWAY_LIMITS, type AwayLimits } from './away';
import { changelog } from './changelog';
import { gamesStarted, playedMs, PlayTime, SMS_AFTER_MS } from './playtime';
import { canKeep } from './pwkeep';
import { keepPageId, PAGE_ID_FILE, pageId, seen } from './page-id';
import { NameClaims, onlineName } from './claim';
import { claimQuietly, NameRow, quietCheck, renamePick, settleName, type PlayNameDeps } from './claim-ui';
import { RenderCheck, RENDER_CHECK_TIMES, type RenderCheckTimes } from './rendercheck';
import { moviesKept, Predownload } from './predownload';
import { movieCodec } from '../host/movie';
import type { Packs } from '../host/datacache';
import { texSplit } from '../gpu/index';

type Phase = 'boot' | 'ready' | 'error' | 'waiting' | 'starting' | 'playing' | 'exited';

const $ = <T extends HTMLElement = HTMLElement>(sel: string) => document.querySelector<T>(sel)!;

// the page address's test aids, once boot knows whether the server is a development server
const TEST_PARAMS = ['env', 'save', 'fresh', 'reg', 'script', 'artifacts', 'dumpscale', 'away', 'rendercheck', 'playtime', 'claim'];
/** a test page's keyphrase token (claim=, or env=NFSU2_TEST_CLAIM=: a development server's only): its claimed name and
 *  friends without cloud saves */
const testClaim = (): string | null => q.get('claim') ?? ENVIRONMENT.NFSU2_TEST_CLAIM ?? null;
const addr = new URLSearchParams(location.search);
let q = new URLSearchParams();
let SAVE = 'save';
let ENVIRONMENT: Record<string, string> = {};
/** the test aids the page address gives (taken on a development server only); their names, for the start page */
function takeParams(dev: boolean): string[] {
  const given = TEST_PARAMS.filter(k => addr.has(k));
  if (!given.length || !dev) return given;
  q = addr;
  // never the game data's own OPFS folder, nor the page id's file (savefs keeps names to one segment)
  const save = (q.get('save') || 'save').replace(/[^A-Za-z0-9._-]/g, '_');
  SAVE = save === 'cache' || save === PAGE_ID_FILE || /^\.+$/.test(save) ? 'save-test' : save;
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
/** Ghost mode as roam_collide has it: on (Ghost mode off), off (Players), none (Everything) */
type Collide = 'on' | 'off' | 'none';
let roamCollide: Collide | null = null;  // the running game's free roam's Ghost mode: as it started, then as the menu changed it (null: no free roam)
let roamCrews: Crew[] | null = null;     // the running game's crews: as it started, then as the menu changed them (null: no free roam)
let roamRoom: string | null = null;      // the running game's room: as it started, then as an invite link changed it ('': none; null: no free roam)
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
let savesLocked = false;                 // the game about to start reads the saves: no cloud version comes in (VaultPanel's playing)
let fatal: { what: string; code?: string } | null = null;   // why the running game had to be stopped (English, for t())
// the last game lost its graphics (GPU-LOST): Safari's WebGPU gives this page no adapter any more, so Play reloads it
let gpuGone = false;
// the Android version for the log's browser line (devicelog.ts androidMajor), asked once as the page loads
const androidVersion = androidMajor();
let fps: FpsOverlay;
let statusAt: { memory: WebAssembly.Memory; ptr: number } | null = null;   // the running game's status block
let radioLink: RadioLink | null = null;  // the radio's words of the running game, once the game says it has them
const radioPort = () => radioLink ??= statusAt ? RadioLink.open(statusAt.memory, statusAt.ptr) : null;
let radio: Radio | null = null;          // made at the first click on Radio: a player who never uses it runs none of it
const theRadio = () => radio ??= new Radio(radioPort, l => log.add(`[radio] ${l}`));
let lobbyPanel: LobbyPanel;
let quickChat: QuickChat;                // free roam's fixed phrases (quickchat.ts)
let friendPrompt: FriendPrompt;          // free roam's friends: Add friend over the game when front bumpers touch (friends.ts)
let friendsPanel: FriendsPanel;          // ...and the friends on the start page
const msgStore = new MessageStore();      // messages between friends (messages.ts): the conversations in this browser
let friendMessages: FriendMessages;       // ...in the game: the log's lines, I and its card
let chatDialog: ChatDialog;               // ...on the start page: one conversation
let settings: SettingsPanel;
let players: { now(): void; last(): PlayerCounts | null; fresh(ms: number): Promise<PlayerCounts | null> } | null = null;
let playersPanel: PlayersPanel;
/** the name the linked passphrase claims, as the line under Your name last heard (undefined: not asked yet) */
let claimedName: string | null | undefined;
let saves: SavesPanel;
let vault: VaultPanel;
/** the claimed names (claim.ts), and the line under Your name (claim-ui.ts) */
let claims: NameClaims;
let nameRow: NameRow;
let download: Predownload;
/** the server does not answer: the page and the module came from this browser's copies (sw.ts), and a game
 *  runs from the game data on this PC */
let offline = false;
/** why the page is offline ('ok' while it is not): what it says (reachText) */
let reach: Reach = 'ok';
/** the server's misses in a row, and whether it answered since the page started (setReach) */
let misses = 0, answered = false;
/** the profile the running (or last) game saved last, when, and how often it saved one */
let saved: { name: string; at: number; count: number } | null = null;
/** the save folder as this game's start had it (saves-ui.ts snapshotSaves): what comes back if the browser deletes the
 *  site's data during the game and there are no cloud saves (saveGone) */
let playStart: FileIn[] = [];

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
    setPhase('error', T('Not ready'), bad.id === 'module' ? T('The game didn\'t load. It retries by itself, or press Retry.') : T('Fix the problem below, then reload this page.'));
    return;
  }
  if (!instance.owner) {
    setPhase('waiting', T('Open elsewhere'), T('The game is running in another tab. Switch to it, or close it and this page takes over.'));
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
  const press = N('Press Play to start.');
  setPhase('ready', T('Ready'), offline ? () => t('No connection to the server: you can play, but not with others. {press}', { press: t(press) }) : T(press));
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
/** the texture packs picked (runtime/web/texrep_web.c), comma-separated, '' for none: the ones the page fetches ahead
 *  and counts.  Each setting's value names its pack */
const texpack = (r: Resolved = resolved()) => [r.env.NFSU2_UI_TEXTURES, r.env.NFSU2_MAP_TEXTURES]
  .map(v => (v ?? '').toLowerCase()).filter(v => v && v !== 'original').join(',');
/** the data the settings pick (datacache.ts Packs): the texture pack and the songs (EA's own: io-worker.ts dataFile) */
const packs = (r: Resolved = resolved()): Packs => ({ texpack: texpack(r), songs: (r.env.NFSU2_SONGS ?? '').trim().toLowerCase() });
const packsKey = () => { const p = packs(); return `${language()}/${p.texpack}/${p.songs}`; };

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
  body.append(p(t('Drive in your career\'s free roam with other players and challenge each other to outruns. Races, traffic and money stay your own.')),
    p(t('Everyone on this server sees your name and whether you\'re driving or in the menus; players near you also see your car.')));
  const c = players?.last();
  if (c?.full === true) {
    const n = Number.isInteger(c.queue) && c.queue! > 0 ? c.queue! : 0;
    const turn = t('You wait in line too, driving alone until it\'s your turn.');
    body.append(p(`${n ? tn(n, 'The server is full: {n} player is in line.', 'The server is full: {n} players are in line.')
      : t('The server is full.')} ${turn}`));
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
  body.append(p(t('You can change this under Online on the start page.')));
  const answer = confirmBox(t('Free roam with others?'), body, t('Turn on'), false, '', t('Not now'));
  $<HTMLButtonElement>('#confirm-ok').focus();
  const on = await answer;
  let text = putKey(settingsText ?? TEMPLATE, 'roam', on ? 'on' : 'off').text;
  const typed = field?.value.trim() ?? name;
  if (on && typed !== name) text = putKey(text, 'roam_name', typed || null).text;
  // (not written, the browser's storage full: this game plays as answered, and the next Play asks again)
  await writeFile(SAVE, 'settings.ini', text).catch(e => log.add(`[app] free roam with others: the answer was not written (${e})`));
  settingsText = text;
  void settings.refresh();
  log.add(`[app] free roam with others: ${on ? 'on' : 'off'} (asked at the first Play${c?.full === true ? ', the server full' : ''})${on && typed !== name ? `, name ${typed || '(the profile\'s)'}` : ''}`);
  return true;
}

/**
 * Play asks a player with a profile here and no cloud saves to turn them on: the saves live only in this browser, and
 * browsers clear a site's data now and then.  First at the first Play with a profile here (a new player's second: the
 * first asks about free roam, and the next one asks), then a week after each Not now (or Esc), three times in all
 * (vault-ui.ts offerable; before 2026-10-31 once, at the fifth Play), and once more past 12 games started and 3 hours
 * played here, whatever the count and the week (2026-11-03).  It shows what is at stake (the profiles, their
 * money, the hours played here; until 2026-11-03 also a Download backup button).  Not on a test page, offline, or where
 * the server keeps no cloud saves.  Turned on, the keyphrase is shown to keep (copy, download) before the game starts;
 * it failed, the reason.
 */
async function askCloud() {
  const games = gamesStarted(), played = playedMs();
  if (testPage() || offline || !vault.offerable(games, played) || !await newestProfile()) return;
  const which = vault.offered(games, played);
  const p = (text: string) => Object.assign(document.createElement('p'), { textContent: text });
  const body = document.createElement('div');
  body.append(p(t('Your saves are only in this browser, and browsers sometimes clear them.')),
    await vault.stake(played),
    p(t('Cloud saves back them up and sync them between your devices.')));
  // the name, when it is free to claim: the new keyphrase claims it at once, and its popup says so; another player's:
  // the keyphrase does not hold it, said before Play's question about the name (2026-11-02, a player's report: given a
  // passphrase, then told the name was claimed)
  const name = onlineName(await typedName());
  const info = name ? await claims.check(name) : null;
  const taken = name && info?.state === 'taken' ? info.name ?? name : null;
  if (name && info?.state === 'free' && !info.mine) body.append(p(t('It also claims your name {name}, so no one else can play under it.', { name })));
  // (the other player may be this one, its passphrase in another browser: a new one here could not get the name back)
  if (taken) body.append(p(t('{name} is claimed by another player, so this passphrase keeps your saves but not that name.', { name: taken })),
    p(t('If it\'s yours, choose Not now and enter your passphrase under Cloud saves.')));
  const answer = confirmBox(t('Turn on cloud saves?'), body, t('Turn on'), false, '', t('Not now'));
  $<HTMLButtonElement>('#confirm-ok').focus();
  const on = await answer;
  log.add(`[app] cloud saves: ${on ? 'turned on' : 'not now'} (asked at Play, ${which})`);
  if (!on) return;
  const phrase = await enableCloud();
  if (!phrase) return;
  const claimed = name && info?.state === 'free' && !info.mine ? await claimQuietly(playDeps(), name, info, quietCheck) : null;
  await showPassphrase(phrase, claimed?.name ?? null, taken);
}

/** Cloud saves turned on (Play's question, or Claim under Your name without a keyphrase): the keyphrase made, or
 *  null, after a box that says why not.  `showPassphrase` then shows it to keep. */
async function enableCloud(): Promise<string | null> {
  const got = await vault.enable();
  if ('why' in got) {
    await confirmBox(t('Cloud saves are not on'), got.why ? t('They could not be turned on: {error}. Try again under Cloud saves on the start page.', { error: got.why })
      : t('They could not be turned on. Try again under Cloud saves on the start page.'), t('Continue'), false, '', '');
    return null;
  }
  return got.phrase;
}

/** A keyphrase just made, shown whole to keep (copy, download): for the saves, and for the name it claimed (null:
 *  none), which it is the only way back to as well; taken: the name another player claimed, which it does not hold */
async function showPassphrase(phrase: string, claimed: string | null, taken: string | null = null) {
  const p = (text: string) => Object.assign(document.createElement('p'), { textContent: text });
  const box = Object.assign(document.createElement('p'), { className: 'keyphrase', textContent: phrase });
  const copy = Object.assign(document.createElement('button'), { type: 'button', className: 'text-btn', textContent: t('Copy') });
  copy.onclick = async () => {
    try { await navigator.clipboard.writeText(phrase); copy.textContent = t('Copied.'); }
    catch { getSelection()?.selectAllChildren(box); }
  };
  const file = Object.assign(document.createElement('button'), { type: 'button', className: 'text-btn', textContent: t('Download passphrase') });
  file.onclick = () => vault.download();
  const row = document.createElement('div'); row.className = 'row keyphrase-do';
  row.append(copy, file);
  const kept = document.createElement('div');
  // what it does (syncs the saves to the other devices it is entered on), then that only the player has it
  if (claimed) kept.append(p(t('{name} is yours, held by this passphrase. Cloud saves are on too.', { name: claimed })));
  kept.append(p(t('Enter it under Cloud saves on another device to sync your saves.')), box, row,
    p(t('Store it somewhere safe: it can\'t be recovered.')));
  if (taken && !claimed) kept.append(p(t('{name} is claimed by another player, so this passphrase keeps your saves but not that name.', { name: taken })));
  // its "Save password?" (vault-ui.ts asked it as the keyphrase was made)
  if (canKeep()) kept.append(p(t('Your browser may also offer to save it with your passwords: choose Save.')));
  const shown = confirmBox(t('Your passphrase'), kept, t('Continue'), false, '', '');
  $<HTMLButtonElement>('#confirm-ok').focus();
  await shown;
}

/** what Play's questions about the name need (claim-ui.ts) */
function playDeps(): PlayNameDeps {
  return {
    claims, seed: pageId, linked: () => vault.linked(), log: logLine,
    choose: (title, body, ok, cancel) => choiceBox(title, body, ok, cancel),
    save: n => settings.putName(n),
  };
}

/** Your name as settings.ini has it, else the newest profile's (what the game goes by) */
async function typedName(): Promise<string> {
  const r = resolved();
  return (r.source.NFSU2_ROAM_NAME === 'settings.ini' ? r.env.NFSU2_ROAM_NAME?.trim() : '') || (await newestProfile())?.name || '';
}

/**
 * The name this game plays under, by the claims (claim-ui.ts settleName): NAME and another player's claimed name get
 * this browser's digits (asked about once), a free name with a keyphrase here is claimed.  null: as it is.  Only
 * where the game meets other players (the network on, free roam or Online mode), and not offline or on a test page.
 */
async function playName(): Promise<string | null> {
  const r = resolved();
  if (testPage() || offline || /^(off|0|none)$/i.test(r.env.NFSU2_NET?.trim() ?? '')) return null;
  if (r.env.NFSU2_ROAM !== '1' && r.env.NFSU2_ONLINE === '0') return null;
  const typed = await typedName();
  const name = await settleName(playDeps(), typed, SAVE, quietCheck);
  if (name && name !== onlineName(typed)) log.add('[app] your name: this game plays under another (digits, or one picked)');
  return name;
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
  if (n) body.append(p(tn(n, '{n} player is waiting in line.', '{n} players are waiting in line.')));
  body.append(p(t('Wait in line: play alone now and join the others when it\'s your turn. The top of the screen shows your place.')),
    p(t('Play offline: no free roam with others or Online this time.')));
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
  try { await startGame(); }
  catch (e) { startFailed(e); }
  finally { starting = false; savesLocked = false; if (!session) vault.noGame(); updatePlay(); }
}

/** the sound of a game being started, until its session has it (startFailed closes it) */
let startAudio: AudioContext | null = null;
/** Play stopped by an error before the game ran (the stage is up from the click on: Play's questions show over it): the
 *  start page back, usable, with what happened */
function startFailed(e: unknown) {
  // (String(e): its name and message in every browser; Firefox's stack has neither)
  log.add(`[app] the game could not start: ${String(e)}${e instanceof Error && e.stack ? `\n${e.stack}` : ''}`);
  if (session) return;
  void startAudio?.close().catch(() => {}); startAudio = null;
  endStage();
  setPhase('exited', T('Did not start'), T('The game could not start: {error}', { error: String(e) }), 'GAME-START');
  $('#log-btn').hidden = $('#report').hidden = false;
  download.resume();
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
  // inside the click: things that need a user gesture (the click's activation: a slow pull of the cloud saves below
  // would spend it), the questions before the game showing over the stage.  The context runs at the mixer's own rate
  // (44.1 kHz), so the worklet does not resample.
  const pre = resolved();
  let audio: AudioContext | null = null;
  try {
    const lat = pre.env.NFSU2_AUDIO_LATENCY ?? 'interactive';
    const hint: AudioContextLatencyCategory | number = /^[0-9.]+$/.test(lat) ? Math.min(0.5, Math.max(0.001, +lat / 1000)) : lat as AudioContextLatencyCategory;
    audio = startAudio = new AudioContext({ latencyHint: hint, sampleRate: 44100 });
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
  let fsNote = '';
  // an iPhone has no full screen for the page (from the Home Screen it has no browser bars to hide)
  if (full && homeScreenInstead()) fsNote = t(HOME_SCREEN);
  else if (full && canFullscreen()) {
    const fs = await enterFullscreen(stage);
    if (!fs.ok) fsNote = t('Full screen was not allowed here; {menu} has it.', { menu: menuName() });
    else if (!fs.locked && !touchFirst()) fsNote = t('Esc leaves full screen in this browser (no keyboard lock).');
  }
  // the keyphrase's newer saves from another browser (4 s at most: what they bring may be a conflict), then a
  // cloud-saves conflict waiting (nothing syncs until it is settled): settled, or no game (the page as it was); before
  // the questions that write settings.ini, which the cloud's saves would replace.  (A first check of the server still
  // under way: its answer first, 5 s at most, so the pull and the questions know the passphrase's sync.)
  await vault.started();
  const pulled = await vault.beforePlay();
  const cloud = await vault.askAtPlay();
  if (cloud === 'stop') { void audio?.close().catch(() => {}); startAudio = null; endStage(); return; }
  if (cloud === 'taken' || pulled) await loadSettings().catch(e => log.add(`[app] settings.ini not read again: ${e}`));
  // (the free-roam question says when the server is full: answered, it is not asked again what to do about that)
  const roamAsked = await askRoam();
  if (!roamAsked) await askCloud();
  const alone = !roamAsked && await askFull(counts);
  // the name this game plays under, by the claims
  const playAs = alone ? null : await playName();
  setPhase('starting', T('Starting'), full && canFullscreen() ? T('Going full screen and starting the game.') : T('Starting the game.'));
  const size = await settledSize(canvas);
  await Promise.race([refreshMeasured, new Promise(r => setTimeout(r, 2000))]);
  canvas.focus();

  // what the Saves section is writing (a rename, a restore) and every restore asked for end first, and no cloud version
  // comes in from here on (VaultPanel's playing): the game reads the saves as they are now
  savesLocked = true;
  await saves.settled();
  // the save overlay: registry seed (bundle.m app_seed_registry), test inputs
  const host: Record<string, string> = {
    HOME: '/save', NFSU2_DATA: '/data', NFSU2_SAVEDIR: '/save', NFSU2_REGFILE: '/save/registry.txt',
    NFSU2_WIDTH: String(size.w & ~1), NFSU2_HEIGHT: String(size.h & ~1), NFSU2_DISPLAY_HZ: String(displayHz),
  };
  const r = resolved(host);
  // no profile yet: the game's name field for the first one starts with Your name, not NAME (overrides.c
  // sub_004ff3b0); the name typed, never the digits this game may play under instead
  const profiles = await listProfiles(SAVE).catch(() => []);
  const firstName = profiles.length || r.source.NFSU2_ROAM_NAME !== 'settings.ini' ? '' : newProfileName(r.env.NFSU2_ROAM_NAME ?? '');
  if (firstName) r.env.NFSU2_NEW_PROFILE_NAME = firstName;
  if (playAs) r.env.NFSU2_ROAM_NAME = playAs;
  // Players' cars shown by this device's default, not the player's pick: the game shows 32 after all once its frames
  // are slow with more (roam_game.c cars_cut)
  if (r.source.NFSU2_ROAM_MAX_CARS === 'device') r.env.NFSU2_ROAM_CARS_AUTO = '1';
  // the room and crews in Unicode's composed form, as the page hashes them (players.ts roomText): one "é" whichever way
  for (const k of ['NFSU2_ROAM_ROOM', 'NFSU2_ROAM_CREW'] as const) if (r.env[k]) r.env[k] = roomText(r.env[k]);
  // the names this game goes by, for the log as it leaves the browser (privacy.ts)
  privacy.name(r.env.NFSU2_ROAM_NAME, 'you');
  for (const c of crewList(r.env.NFSU2_ROAM_CREW ?? '')) privacy.name(c, 'crew');
  privacy.name(r.env.NFSU2_ROAM_ROOM, 'room');
  for (const p of profiles) privacy.name(p.name, 'profile');
  if (firstName) privacy.name(firstName, 'profile');
  // offline mode: the server does not answer.  The movies stream from it, so unless every one of them is in this
  // browser they are off for this game (a movie the game could not read would stop it).
  setReach(await serverAnswers(2000));
  // the movies in AV1 where this browser decodes it, else in VP6 (host/movie.ts; settings.ini NFSU2_MOVIE_CODEC)
  r.env.NFSU2_MOVIE_CODEC = await movieCodec(r.env);
  let offlineNote = '';
  if (offline) {
    log.add('[app] offline: the server does not answer, the game runs from this PC (no free roam or Online)');
    if (r.env.NOMOVIES !== '1' && !await moviesKept(language(r), r.env.NFSU2_MOVIE_CODEC === 'av1').catch(() => false)) {
      r.env.NOMOVIES = '1';
      log.add('[app] offline: not every movie is on this PC, so the movies are off for this game');
      offlineNote = t('No connection to the server: the movies are off for this game.') + ' ';
    }
  }
  // the log is for sharing when asking for help: it names neither the server (a private server's address) nor
  // more of this browser than its major version and system (the audit's PRIV-5).  These session lines stay when
  // a long game fills the log (logs.ts keep).
  log.keep(`=== NFSU2 web, ${new Date().toISOString()}, game ${loaded!.info.rev}, server ${serverKind()} ===`);
  log.keep(`[app] save folder: OPFS ${SAVE} (/save); canvas ${size.w}x${size.h} device pixels, display ${displayHz} Hz; ${browserBrief(navigator.userAgent, navigator.maxTouchPoints, phoneScreen(navigator.maxTouchPoints), await androidVersion)}; ${checks.get('gpu')?.label ?? 'WebGPU'}; movies in ${r.env.NFSU2_MOVIE_CODEC === 'av1' ? 'AV1' : 'VP6'}`);
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
      const a = await oneWriter(() => turnOnAutosave(filesOf(SAVE), PROFILES_DIR));     // (after any restore, never beside one)
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
  roamCollide = net && r.env.NFSU2_ROAM === '1' ? collideOf(r.env.NFSU2_ROAM_COLLIDE) : null;
  roamCrews = net && r.env.NFSU2_ROAM === '1' ? crewsOf(r.env.NFSU2_ROAM_CREW ?? '', r.env.NFSU2_ROAM_CREW_RGB ?? '') : null;
  roamRoom = net && r.env.NFSU2_ROAM === '1' ? r.env.NFSU2_ROAM_ROOM ?? '' : null;
  crewDraft = ''; popCounts = new Map();
  fatal = null;
  quitAt = null;
  saved = null;
  playStart = await snapshotSaves(SAVE).catch(() => []);
  try {
    // the callbacks are module-level functions, not closures made here: the
    // session's subsystems keep them (some, like a closed AudioContext, a little
    // longer than the game), and a closure from this function would keep its
    // scope, and with it this game, reachable
    const ids = net ? await keepPageId() : null;
    session = await GameSession.start({
      module: loaded!.module, info: loaded!.info, saveDir: SAVE, env: r.env, canvas, stage, audio, displayHz, netName, notice, lobby, say, marks, bump, battle,
      friend: friendMsg,
      netBack, netQueue, onSaved, onSaveGone: saveGone, onRemoved, onLost,
      netId: ids?.id, netWas: ids?.was, netClaim: net ? testClaim() ?? await vault.token().catch(() => null) ?? undefined : undefined, netRemoved, netRaces, netRename,
      netBlock: net && r.env.NFSU2_ROAM === '1' ? blockList(r.env.NFSU2_ROAM_BLOCK ?? '') : undefined,
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
  startAudio = null;                      // the session's now
  setPhase('playing', T('Playing'), () => '');
  if (!offline && !testPage()) seen('play');   // counted for the server's day (page-id.ts)
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
  friendPrompt.start(session, net && r.env.NFSU2_ROAM === '1');
  friendMessages.start(session, net && r.env.NFSU2_ROAM === '1');
  // (the races' between players free roam on or not; free roam's only with it on: the career's free roam alone keeps
  // 1-4, Esc and the right stick's click for the game)
  quickChat.start(session, net && r.env.NFSU2_ROAM_SAY !== '0', net && r.env.NFSU2_ROAM === '1',
    phrasesOf(r.env.NFSU2_ROAM_SAY_PHRASES), groupsOf(r.env.NFSU2_ROAM_SAY_GROUPS), blockList(r.env.NFSU2_ROAM_BLOCK ?? ''));
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
  if (autosaved.length) toast(t('Autosave is now on for {names} (Options, Gameplay, Save). {hint}', { names: listOf(autosaved), hint }), 9000);
  else toast(hint, 6000);
  void st.exited.then(code => { clearInterval(waitStatus); onExit(st, code); });
  // Chrome may keep the sound off until a click or a key press (its autoplay
  // rule); a controller button does not count, so say what does
  setTimeout(() => {
    if (session !== st || !st.audio || st.audio.state() === 'running' || r.env.NOSOUND === '1') return;
    log.add(`[app] the AudioContext is ${st.audio.state()} after Play`);
    toast(touchFirst() ? t('No sound yet: tap the game (a controller button can\'t turn it on).')
      : t('No sound yet: click the game or press a key (a controller button can\'t turn it on).'), 8000);
  }, 3000);
}

function logLine(s: string) { log.add(s); }
/** whether the server answered (the start page's /players, or the check at Play): offline mode when not.  One miss
 *  after it answered is not offline yet (an answer that was slow, a phone's network gone for a moment): the next
 *  question decides, 15 s later on the start page, or the check at Play. */
function setReach(r: Reach) {
  misses = r === 'ok' ? 0 : misses + 1;
  if (r === 'ok') answered = true;
  else if (answered && misses < 2 && r !== 'no-network') { log.add(`[app] the server did not answer once (${REACH_LOG[r]})`); return; }
  if (r === reach) return;
  reach = r;
  offline = r !== 'ok';
  log.add(offline ? `[app] the server does not answer: offline (${REACH_LOG[r]})` : '[app] the server answers again');
  if (offline) playersPanel.offline(REACH_TEXT[r]);
  evaluate();
  if (offline) return;
  // the page may have started from offline mode's copies while the server was away (a deploy restarts it)
  void checkNewer();
  void vault.start();                 // the cloud saves, when the server did not answer as the page started
  players?.now();                     // the Playing now panel's numbers again (offline's line, after the check at Play)
}
const REACH_LOG: Record<Reach, string> = {
  ok: '', down: 'a 5xx: the server is down or restarting', unreachable: 'no answer: blocked on the way, or too slow',
  'no-network': 'the browser says this device has no network',
};
/** the Playing now panel's line while offline, by why */
const REACH_TEXT: Record<Reach, Msg> = {
  ok: () => '',
  down: T('The server isn\'t answering: it may be restarting. Checking again every 15 seconds.'),
  unreachable: T('This device can\'t reach the server. If other sites work, your network may be blocking it: try mobile data or another Wi-Fi, or turn off a VPN, ad blocker or private DNS. Checking again every 15 seconds.'),
  'no-network': T('No internet connection.'),
};
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
const HOME_SCREEN = N('For full screen on iPhone, add this page to the Home Screen (Share menu) and play from its icon.');
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

/** the buttons at the top: without their icons where one's word doesn't fit beside its icon (a phone's two by two,
 *  style.css), with them again where every word does.  Measured as the contents' width against the button's, less
 *  8 px a side (the padding without icons): a button centres what it holds, so too much reaches into its padding on
 *  both sides, and no scrollWidth shows it */
function fitTop() {
  const top = $('.top-end'), r = document.createRange();
  top.removeAttribute('data-tight');
  const tight = [...top.querySelectorAll<HTMLElement>('.btn')].some(b => {
    r.selectNodeContents(b);
    return r.getBoundingClientRect().width > b.clientWidth - 16;
  });
  top.toggleAttribute('data-tight', tight);
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
  click: () => quickChat.click(),
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
  friendPrompt.stop();
  friendMessages.stop();
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
  for (const id of ['#crews-pop', '#friends-pop', '#menu']) { const d = $<HTMLDialogElement>(id); if (d.open) d.close(); }
  // a Quit question still open belongs to the game that ended (its answer would
  // do nothing, and the open question would keep that game reachable)
  const ask = $<HTMLDialogElement>('#confirm'); if (ask.open && ask.dataset.about === 'quit') ask.close();
  $('#stage').hidden = true;
  $('#stage').querySelector('canvas')?.remove();
  $('#app').inert = false;
  clearToasts();
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
  if (gpuGone) setPhase('exited', T('Stopped'), () => t('The game was stopped: {what}. Play reloads the page; then press Play again. Your saves and settings stay.',
    { what: t(f!.what) }) + ' ' + t('Download the log first if you want to keep it: the reload clears it.'), f!.code);
  // the phone's graphics driver refused the picture at the first frame (game.ts): another Play ends the same way
  else if (f?.code === 'GPU-ANDROID-DRIVER') setPhase('exited', T('Did not start'), () => t('{browser} cannot show the game with this phone\'s graphics driver. No setting changes that; a system update for the phone may.',
    { browser: t(browserName()) }), f.code);
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
  void friendsPanel.refresh(true);    // friends made in the game
  showNewer();                        // the note on the log, now that there is one
  void checkNewer();
}

/**
 * The browser deleted this site's data during the game (low disk space, or its own settings; Discord 2026-10-31: every
 * save after it failed): the I/O worker made the save folder again, empty, and holds the game's saves until the saves
 * are back in it (io-worker.ts recover): the cloud's, else those of the game's start (VaultPanel.refill).  The player
 * is told what came back.
 */
async function saveGone(): Promise<void> {
  log.add('[app] the browser deleted this site\'s data during the game: the saves go back into the empty save folder');
  // settings.ini as the page has it now (it writes the file whenever it changes it)
  if (settingsText !== null) void snapshotFile(playStart, SAVE, 'settings.ini', new TextEncoder().encode(settingsText));
  let msg: string;
  try {
    const r = await vault.refill(playStart);
    log.add(`[app] the saves are back (${r.files} files): ${r.from === 'cloud' ? 'the cloud\'s, with this device\'s own files' : 'as the game\'s start had them'}`);
    msg = r.from === 'cloud'
      ? t('The browser deleted this site\'s data during the game. Your cloud saves are back, and the game saves on top of them. Screenshots and covers are gone.')
      : vault.linked() ? t('The browser deleted this site\'s data during the game. Your profiles and settings are back as this game last saved them; screenshots and covers are gone.')
      : t('The browser deleted this site\'s data during the game. Your profiles and settings from the start of this game are back; screenshots and covers are gone. Cloud saves or a backup (Saves) keep your progress safe.');
  } catch (e) {
    // the saves of the game's start as a backup in the downloads: Restore takes it back
    log.add(`[app] the saves could not be put back: ${e}; their backup downloaded`);
    try { downloadBlob(zip(playStart.map(f => ({ name: f.path, data: f.data, mtime: f.mtime }))) as BlobPart, `nfsu2-saves-${stamp()}.zip`, 'application/zip'); }
    catch { /* nothing more to do */ }
    msg = t('The browser deleted this site\'s data during the game, and the saves could not all be put back ({error}). A backup of them was downloaded: Restore takes it back (Saves).', { error: String(e) });
  }
  if (session) toast(msg, 15000, true);
}

/** a file the game wrote as the browser deleted the save folder, not kept (its bytes went with the folder): the player
 *  saves again, as no "Progress saved" said so */
function onLost(path: string) {
  log.add('[app] a save the game wrote as the browser deleted the save folder was not kept: asked to save again');
  if (session && path.startsWith(PROFILES_DIR + '/')) toast(t('Your last save may not have been kept: the browser deleted this site\'s data as it was written. Save again in the game.'), 15000, true);
}

/** the game deleted a file of the save folder: a profile's (its own Delete), which no refill brings back then */
function onRemoved(path: string) {
  const p = path.split('/');
  if (p.length !== 3 || p[0] !== PROFILES_DIR || p[2].toLowerCase() !== p[1].toLowerCase() || coverOf(p[1])) return;
  log.add(`[app] the game deleted profile ${p[1]}`);
  const i = playStart.findIndex(f => f.path === path);
  if (i >= 0) playStart.splice(i, 1);
  vault.removed();
}

/** the game finished writing a file of the save folder: a profile is the player's progress */
function onSaved(path: string) {
  void snapshotFile(playStart, SAVE, path);                // what comes back if the browser deletes the folder (saveGone)
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

/** the controls sheet of the start page, in a dialog (F9, Controls, while the game runs), at the page's tab (or the
 *  one asked for: the touch controls' steering button held opens the touch settings) and button names */
let keySheet: ControlsSheet, helpSheet: ControlsSheet | null = null;
function openHelp(tab?: Tab) {
  const body = $('#help-body');
  if (!helpSheet) {
    body.append(...[...$('#keys').childNodes].map(n => n.cloneNode(true)));
    for (const k of body.querySelectorAll('[data-keep]')) k.remove();   // (the start page's Controller settings: Play reads them)
    helpSheet = controlsSheet(body, 'help');
  }
  helpSheet.show(tab ?? keySheet.tab);
  helpSheet.family(keySheet.fam);
  $<HTMLDialogElement>('#help').showModal();
}

/** a line for the player from the network (also in the log): the page's own (net/index.ts: the server full, a place
 *  freed up) and the game's (alone in a room) in the toast over the game, as the page's other messages; a friend
 *  joined, left, went into a race (who: the friend's name the text starts with) in the notices' box, four at most,
 *  each for 5 s.  Friends' lines are only logged for now: with the crowds on the server they came too often */
const FRIEND_NOTICES = false;
function notice(text: string, who?: string) {
  privacy.name(who, 'player');
  log.add(`[notice] ${text}`);
  if (!who) { toast(t(text), 6000); return; }     // English in the log
  if (!FRIEND_NOTICES) return;
  const box = $('#notices'), line = document.createElement('div');
  if (text.startsWith(who)) { const b = document.createElement('b'); b.textContent = who; line.append(b, text.slice(who.length)); }
  else line.textContent = text;
  placeNotices();
  box.append(line);
  while (box.childElementCount > 4) box.firstElementChild!.remove();
  setTimeout(() => line.remove(), 5000);
}
/** a person: what marks a line in the log as a friend's (friendNotice), as the speech balloon marks quick chat's */
const FRIEND_ICON = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M8 8a3 3 0 1 0 0-6 3 3 0 0 0 0 6zM2 15c0-3.3 2.7-5.5 6-5.5s6 2.2 6 5.5z"/></svg>';
/** free roam: a friend (friends.ts: the server's friends lists) came into free roam: a line in the log as quick chat's
 *  (chatLine), a person first, the text with the friend's name in bold; these come seldom, so they show, where other
 *  players' joins are only logged */
function friendNotice(name: string, text: string) {
  privacy.name(name, 'player');
  log.add(`[notice] ${name} is in free roam (a friend)`);
  const b = document.createElement('b'), at = text.indexOf(name);
  b.textContent = name;
  placeNotices();
  iconLine('chat', FRIEND_ICON, at < 0 ? [text] : [text.slice(0, at), b, text.slice(at + name.length)]);
}
/** quick chat (quickchat.ts) in free roam and races between players: a phrase said, from the game; and free roam's
 *  lines over the cars in each frame */
function say(json: string) {
  placeNotices();
  quickChat.said(json);
}
function marks(n: number, m: Float32Array, ids: Uint32Array) { quickChat.marks(n, m, ids); }
/** free roam's friends: front bumpers touching (the game), and the server's friend messages (friends.ts) */
function bump(json: string) { friendPrompt.bump(json); }
/** a drift battle's scoreboard from the game (drift.ts) */
function battle(json: string) { driftPanel.battle(json); }
function friendMsg(m: Record<string, unknown>) { friendPrompt.server(m); friendMessages.server(m); }
/** quick chat's line in the log: a speech balloon, the name, the phrase */
function chatLine(name: string, text: string, me: boolean) {
  const b = document.createElement('b');
  b.textContent = name;
  iconLine('chat' + (me ? ' me' : ''), CHAT_ICON, [b, ` ${text}`]);
}
/** a line in the notices' box with an icon first (quick chat's, a friend's), for 8 s */
function iconLine(cls: string, icon: string, parts: (string | Node)[]) {
  const box = $('#notices'), line = document.createElement('div'), span = document.createElement('span');
  line.className = cls;
  line.innerHTML = icon;
  span.append(...parts);
  line.append(span);
  box.append(line);
  while (box.childElementCount > 4) box.firstElementChild!.remove();
  setTimeout(() => line.remove(), 8000);
}
/** a friend's message in the log (messages.ts): an envelope, the name, the text wrapping under the name; the player's
 *  own as "To <name>" with a note after it; on a touch screen a tap on a friend's answers it.  In the notices' box, for
 *  ms (the line fades out at its end) */
function msgLine(name: string, text: string, out: boolean, note: string, ms: number, tap?: () => void): HTMLElement {
  placeNotices();
  const box = $('#notices'), line = document.createElement('div'), b = document.createElement('b');
  line.className = 'chat msg' + (out ? ' me' : '');
  line.style.animationDuration = `${ms}ms`;
  line.innerHTML = MSG_ICON;
  b.textContent = name;
  line.append(b, ` ${text}`);
  if (note) line.append(Object.assign(document.createElement('i'), { textContent: ` ${note}` }));
  if (tap && touchFirst()) { line.classList.add('tap'); line.addEventListener('click', tap); }
  box.append(line);
  while (box.childElementCount > 4) box.firstElementChild!.remove();
  setTimeout(() => line.remove(), ms);
  return line;
}
/** "I Write back" under the newest friend's message (messages.ts), for ms; null: gone */
let msgHintTimer = 0;
function msgHint(ms: number | null) {
  const box = $('#notices');
  box.querySelector('.msg-hint')?.remove();
  clearTimeout(msgHintTimer);
  if (ms === null) return;
  const h = document.createElement('div'), k = document.createElement('kbd');
  h.className = 'msg-hint';
  h.style.animationDuration = `${ms}ms`;
  k.textContent = MSG_KEY_LABEL;
  h.append(k, t('Write back'));
  box.append(h);
  msgHintTimer = window.setTimeout(() => h.remove(), ms);
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
  friendMessages?.place(left, top, u);         // and messages.ts's card
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
/** an end the operator set, in the page's language: "4 Nov 2026, 14:05" */
const until = (ms: number) => new Date(ms).toLocaleString(dateLocale(), { dateStyle: 'medium', timeStyle: 'short' });
/** the operator's message (in its own words), as a sentence after the page's */
const said = (msg: string) => msg ? ` ${msg}${/[.!?]$/.test(msg) ? '' : '.'}` : '';
function netRemoved(r: NetRemoved) {
  const head = !r.ban ? t('You were removed from playing with others.')
    : r.until ? t('You can\'t play with others until {when}.', { when: until(r.until) })
    : t('You can\'t play with others any more.');
  removedLine(`${head}${said(r.msg)} ${t('You can keep playing on your own.')}`);
}
/** the operator's race ban (net/index.ts onRaces): why and until when, over the game (again when the LAN or Online
 *  screens open); null: lifted */
function netRaces(r: NetRaces | null) {
  if (!r) { toast(t('You can race other players again.'), 6000); return; }
  const head = r.until ? t('You can\'t race other players until {when}.', { when: until(r.until) }) : t('You can\'t race other players any more.');
  toast(`${head}${said(r.msg)} ${t('Free roam stays open.')}`, 12_000);
}
/** the operator took the player's name away (net/index.ts onRename): a question for another (claim-ui.ts renamePick);
 *  picked, the game plays under it at once (the input's name) and the page goes back to the others; Not now: playing
 *  with others waits for another name, as the line over the game says */
let renaming = false;
async function netRename(r: NetRename) {
  privacy.name(r.name, 'you');
  if (renaming) return;
  renaming = true;
  try {
    const name = await renamePick(playDeps(), r.name, r.msg);
    if (!name) {
      log.add('[app] your name: none picked (playing with others waits for another name)');
      removedLine(t('Playing with others waits for another name: change it under Online on the start page.'));
      return;
    }
    privacy.name(name, 'you');
    log.add(`[app] your name: another picked (${session?.input?.name?.(name) ? 'the game plays under it now' : 'the game did not take it'})`);
    session?.net?.rename?.(name);
    hideRemoved();
    void settings.refresh();
  } finally {
    renaming = false;
  }
}
/** the line over the game for a player off the others, until the player plays on (an input REMOVED_MS after it came
 *  up) */
function removedLine(text: string) {
  const box = $('#removed');
  box.textContent = text;
  box.hidden = false;
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

/**
 * A short message on the game, on the first of its two lines that holds none the player must read.  keep: one the
 * player must read (the saves came back after the browser deleted them, a save not kept): nothing replaces it before
 * its time is up ("Progress saved", which the recovery lets go just after it, shows on the other line).  Both lines
 * holding such ones, it waits (another to read in turn; of the others the newest only).
 */
const TOAST_LINES = ['toast', 'toast-also'];
const toastTimers = new Map<string, ReturnType<typeof setTimeout>>(), toastKept = new Set<string>();
const toastsWaiting: { msg: string; ms: number; keep: boolean }[] = [];
function toast(msg: string, ms: number, keep = false) {
  const line = TOAST_LINES.find(id => !toastKept.has(id));
  if (line) { showToast(line, msg, ms, keep); return; }
  if (keep) toastsWaiting.splice(toastsWaiting.filter(w => w.keep).length, 0, { msg, ms, keep });
  else toastsWaiting.splice(0, toastsWaiting.length, ...toastsWaiting.filter(w => w.keep), { msg, ms, keep });
}
function showToast(id: string, msg: string, ms: number, keep: boolean) {
  const el = $(`#${id}`);
  el.textContent = msg; el.hidden = false;
  if (keep) toastKept.add(id); else toastKept.delete(id);
  clearTimeout(toastTimers.get(id));
  toastTimers.set(id, setTimeout(() => {
    el.hidden = true; toastKept.delete(id);
    const next = toastsWaiting.shift();
    if (next) showToast(id, next.msg, next.ms, next.keep);
  }, ms));
}
/** no toast left over from a game that ended */
function clearToasts() {
  for (const t of toastTimers.values()) clearTimeout(t);
  toastTimers.clear(); toastKept.clear(); toastsWaiting.length = 0;
  for (const id of TOAST_LINES) $(`#${id}`).hidden = true;
}

/** F12 (photo.ts: photo mode's picture, or where it cannot open): the next frame the renderer draws (photo mode's: its
 *  still picture once it has all its frames, with its look and crop), as a PNG beside the saves (Saves on the start
 *  page shows it), with the site's address in a corner unless photo mode's panel turned it off */
async function screenshot(photo = false) {
  const r = session?.renderer;
  if (!r?.dump) { toast(t('No screenshot: nothing on screen yet.'), 2500); return; }
  try {
    const png = await (photo && r.photo ? r.photo('screenshot', photoMark()) : r.dump('screenshot', photoMark()));
    if (!png.size) throw new Error('the game ended first');
    const name = await saveScreenshot(SAVE, png);
    log.add(`[app] screenshot ${name}, ${fmtBytes(png.size)}`);
    toast(t('Screenshot saved (see Saves on the start page).'), 2500);
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
  solid.hidden = roamCollide === null || !session?.net?.solid;
  solid.textContent = ghostLabel();
  const crews = m.querySelector<HTMLButtonElement>('[data-act="crews"]')!;
  crews.hidden = roamCrews === null || !session?.net?.crews;
  crews.textContent = crewsLabel();
  m.querySelector<HTMLButtonElement>('[data-act="friends"]')!.hidden = !friendPrompt.hasFriends();
  const fr = m.querySelector<HTMLButtonElement>('[data-act="friend"]')!, item = friendPrompt.menuItem();
  fr.hidden = !item;
  if (item) fr.textContent = item.label;
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
const ghostLabel = () => roamCollide === 'none' ? t('Ghost mode: everything') : roamCollide === 'off' ? t('Ghost mode: players') : t('Ghost mode: off');
/** roam_collide's value from its variable (a test's page address may give 1 or 0; none set: on, the default) */
const collideOf = (v: string | undefined): Collide => { const c = checkSetting('roam_collide', v ?? ''); return c?.ok ? c.value as Collide : 'on'; };
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
    onDraft: v => { crewDraft = v; }, change: (list, f) => void menuCrews(list, f),
    // with the room the game is in (crew-mates meet within one room); what the copy did goes in the popup's note
    link: name => inviteLink(location.origin, { crew: name, room: roamRoom ?? '' }) }, box).setAttribute('aria-labelledby', 'crews-pop-title');
  if (had && focus) ((focus(box) ?? box.querySelector('input:enabled')) as HTMLElement | null)?.focus({ focusVisible: true } as FocusOptions);
}
/** a Leave, Join or colour in the popup: the game takes the crews and their colours at once (its HELLO again to the
 *  hub), and settings.ini keeps them for the next game, as the Crews setting would */
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

/** The menu's Friends popup (free roam, with friends), over the menu: the friends in free roam, a Join for each one in
 *  another room (the game switches to it at once, as an invite link's Join does), the others in a line.  Asked of the
 *  server as it opens, drawn again with its answer; Done, Esc or a controller's B closes it, back on the menu's Friends
 *  button. */
function openFriends() {
  const pop = $<HTMLDialogElement>('#friends-pop');
  if (!friendPrompt.hasFriends() || pop.open) return;
  drawPopFriends();
  pop.showModal();
  ($('#friends-pop-box').querySelector<HTMLElement>('button') ?? pop.querySelector<HTMLElement>('[data-act="friends-done"]'))?.focus();
  friendPrompt.askWhere();
}
function drawPopFriends() {
  const box = $('#friends-pop-box'), had = box.contains(document.activeElement);
  friendPrompt.drawPlaced(box, room => void joinFriend(room));
  if (had) ($('#friends-pop-box').querySelector<HTMLElement>('button') ?? $('#friends-pop').querySelector<HTMLElement>('[data-act="friends-done"]'))?.focus();
}
/** a Join in the menu's Friends: that friend's room, at once, and back to the game */
async function joinFriend(room: string) {
  for (const id of ['#friends-pop', '#menu']) { const d = $<HTMLDialogElement>(id); if (d.open) d.close(); }
  log.add('[app] the menu\'s Friends: joining a friend\'s room');
  tell(joinedText(await applyInvite({ room })));
}

// --------------------------------------------------------- invite links --
// A link's room or crew (invite.ts), offered to the player and joined only on Join.  The tab that owns the saves joins
// it: in settings.ini for the next Play, and with a game in free roam in that game at once (a crew as the menu's
// Crews joins one, a room through net setRoom).  A tab that does not own them (the link
// opened a new tab while the game runs in another) asks that one to over INVITE_CHANNEL and says what it did; when it
// does not answer (a page of before), this tab joins once it owns the saves.
type Joined = 'live' | 'next' | 'full' | 'none';
const INVITE_CHANNEL = `nfsu2-invite:${SAVE}`;
/** the room, free roam and the crews as a settings.ini has them */
interface InviteNow { room: string; roam: boolean; crews: Crew[] }
function inviteNow(text: string | null): InviteNow {
  const e = resolveEnv(text, {}, {}, DEVICE).env;
  return { room: e.NFSU2_ROAM_ROOM ?? '', roam: e.NFSU2_ROAM === '1', crews: crewsOf(e.NFSU2_ROAM_CREW ?? '', e.NFSU2_ROAM_CREW_RGB ?? '') };
}
/** what inv changes there: the room, a crew it is not in yet (full: with 4 already), free roam off to on; and game:
 *  the running game's free roam is not in them yet (a Room changed on the page during the game: the file has it) */
function inviteDiff(inv: Invite, now: InviteNow) {
  const crew = !!inv.crew && !now.crews.some(c => sameName(c.name, inv.crew!));
  const game = !!session && roamCrews !== null && ((inv.room !== undefined && !sameName(inv.room, roamRoom ?? ''))
    || (!!inv.crew && !roamCrews.some(c => sameName(c.name, inv.crew!))));
  return { room: inv.room !== undefined && !sameName(inv.room, now.room), crew, roam: !now.roam, full: crew && now.crews.length >= CREW_MAX, game };
}
/** inv changes nothing: the file and the running game have it all */
const inviteNone = (d: ReturnType<typeof inviteDiff>) => !d.room && !d.crew && !d.roam && !d.game;
/** a line for the player: over the game while one runs, else on the page */
const tell = (msg: string) => session ? toast(msg, 6000) : pageToast(msg);
const JOINED_NOW = N('Your game switches to it now.'), JOINED_NEXT = N('Applies the next time you press Play.');
const CREWS_FULL = N('You are in 4 crews already: leave one under Online, Crews, then open the link again.');
function joinedText(how: Joined): string {
  if (how === 'full') return t(CREWS_FULL);
  return how === 'none' ? t('Joined.') : `${t('Joined.')} ${t(how === 'live' ? JOINED_NOW : JOINED_NEXT)}`;
}
/** whether the running game takes inv at once: it runs free roam (whatever the file says now) */
function inviteLive(): boolean {
  return !!session && roamCrews !== null;
}

/** Join inv here (this tab owns the saves): settings.ini, then the running game where it can. */
async function applyInvite(inv: Invite): Promise<Joined> {
  const was = await readText(SAVE, 'settings.ini').catch(() => null) ?? settingsText ?? TEMPLATE;
  const now = inviteNow(was), d = inviteDiff(inv, now);
  if (d.full) return 'full';
  if (inviteNone(d)) return 'none';
  let text = was, crews = now.crews;
  if (d.roam) text = putKey(text, 'roam', 'on').text;
  if (d.room) text = putKey(text, 'roam_room', inv.room || null).text;
  if (d.crew) {
    crews = [...crews, { name: inv.crew!, rgb: freeColour(crews) }];
    text = putKey(text, 'roam_crew', crewsValue(crews.map(c => c.name))).text;
    text = putKey(text, 'roam_crew_colours', rgbsValue(crews)).text;
  }
  if (text !== was) {
    await writeFile(SAVE, 'settings.ini', text);
    settingsText = text; void settings.refresh(); players?.now(); vault?.changed();
  }
  // (the names, for the log as it leaves the browser: privacy.ts)
  privacy.name(inv.room, 'room'); if (inv.crew) privacy.name(inv.crew, 'crew');
  const live = inviteLive();
  if (live && session?.net && roamCrews && inv.crew && !roamCrews.some(c => sameName(c.name, inv.crew!)) && roamCrews.length < CREW_MAX) {
    roamCrews = [...roamCrews, { name: inv.crew, rgb: freeColour(roamCrews) }];
    session.net.crews?.(roamCrews.map(c => roomHash(c.name)), rgbWords(roamCrews));
  }
  if (live && inv.room !== undefined && !sameName(inv.room, roamRoom ?? '') && session?.net?.room?.(roomText(inv.room))) {
    roamRoom = inv.room;
    friendPrompt.roomChanged();
  }
  log.add(`[app] invite link: joined${d.room ? ` room "${inv.room}"` : ''}${d.crew ? ` crew "${inv.crew}"` : ''}${d.roam ? ', free roam on' : ''}` +
    ` (${live ? 'the game takes it now' : 'from the next Play'})`);
  return live ? 'live' : 'next';
}

/** Join inv in the tab that owns the saves; null: no answer within 3 s (no such tab, or a page of before). */
function relayInvite(inv: Invite): Promise<Joined | null> {
  return new Promise(res => {
    const ch = new BroadcastChannel(INVITE_CHANNEL), id = crypto.randomUUID();
    const done = (v: Joined | null) => { clearTimeout(timer); ch.close(); res(v); };
    const timer = setTimeout(() => done(null), 3000);
    ch.onmessage = e => { if (e.data?.t === 'joined' && e.data.id === id) done(e.data.how as Joined); };
    ch.postMessage({ t: 'join', id, inv });
  });
}
/** The owner's side of relayInvite: a join another tab of this browser asked for (its player said Join there). */
function hearInvites() {
  const ch = new BroadcastChannel(INVITE_CHANNEL);
  ch.onmessage = async e => {
    const m = e.data;
    if (m?.t !== 'join' || typeof m.id !== 'string' || !instance.owner) return;
    const inv: Invite = {};
    if (typeof m.inv?.room === 'string') inv.room = m.inv.room;
    if (typeof m.inv?.crew === 'string') inv.crew = m.inv.crew;
    if ((!inv.room && !inv.crew) || !checkInvite(inv).ok) return;
    const how = await applyInvite(inv);
    ch.postMessage({ t: 'joined', id: m.id, how });
    tell(joinedText(how));
  };
}

/** An invite link opened (the page's address, or a new one pasted while it is open): the question, then the join. */
async function offerInvite(inv: Invite) {
  // once this tab knows whether it owns the saves, has read them, and shows no other question
  await Promise.race([instance.whenOwner, new Promise(r => setTimeout(r, 500))]);
  if (instance.owner && settingsText === null) await loadSettings().catch(() => null);
  while (document.querySelector('dialog[open]')) await new Promise(r => setTimeout(r, 500));
  const ok = checkInvite(inv);
  if (!ok.ok) {
    log.add(`[app] invite link refused: ${ok.why}`);
    tell(t('This invite link cannot be used: {why}.', { why: tvars(ok.why, ok.v).replace(/[.。]$/, '') }));
    return;
  }
  const owner = instance.owner;
  const now = inviteNow(owner ? settingsText : await readText(SAVE, 'settings.ini').catch(() => null)), d = inviteDiff(inv, now);
  const v = { room: inv.room ?? '', crew: inv.crew ?? '', old: now.room };
  // (as the player's own settings spell it)
  if (inviteNone(d)) {
    tell(inv.crew ? t('You are in crew "{crew}" already.', { crew: now.crews.find(c => sameName(c.name, inv.crew!))?.name ?? inv.crew })
      : t('You are in room "{room}" already.', { room: now.room || inv.room! }));
    return;
  }
  const title = !inv.crew ? t('Join room "{room}"?', v) : inv.room ? t('Join crew "{crew}" in room "{room}"?', v) : t('Join crew "{crew}"?', v);
  const body = document.createElement('div');
  const p = (text: string, cls = '') => { body.append(Object.assign(document.createElement('p'), { textContent: text, className: cls })); };
  if (d.full) { p(t(CREWS_FULL)); await choiceBox(title, body, t('OK'), ''); return; }
  if (inv.room) p(t('Free roam then shows only the players in room "{room}".', v) + (d.room && now.room ? ' ' + t('You leave room "{old}".', v) : ''));
  else if (d.room) p(t('You leave room "{old}", so free roam shows everyone again.', v));
  if (inv.crew) p(t('Crew-mates always see each other in free roam, with names and map arrows in the crew\'s colour.'));
  if (d.roam) p(t('This turns on Free roam with others: everyone on this server sees your name, and whether you\'re driving or in the menus.'));
  p(!owner ? t('The game is open in another tab of this browser: it joins there.') : t(inviteLive() ? JOINED_NOW : JOINED_NEXT), 'hint');
  if (session) $('#stage').classList.add('cursor');
  const yes = await confirmBox(title, body, t('Join'), false, '', t('Not now'));
  if (session && !$<HTMLDialogElement>('#menu').open) { $('#stage').classList.remove('cursor'); $('#stage').querySelector('canvas')?.focus(); }
  if (!yes) { log.add('[app] invite link: not now'); return; }
  if (instance.owner) { tell(joinedText(await applyInvite(inv))); return; }
  const how = await relayInvite(inv);
  if (how) { tell(how === 'full' || how === 'none' ? joinedText(how) : `${t('Joined in the other tab.')} ${t(how === 'live' ? JOINED_NOW : JOINED_NEXT)}`); return; }
  tell(t('The other tab did not answer. Close it: this tab joins when it takes over.'));
  void instance.whenOwner.then(async () => { await loadSettings().catch(() => null); tell(joinedText(await applyInvite(inv))); });
}
/** the invite in the page's address, if any, taken out of it (a reload does not ask again), and offered */
function takeInvite() {
  const inv = readInvite(location.hash);
  if (!inv) return;
  history.replaceState(history.state, '', location.pathname + location.search);
  void offerInvite(inv);
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
      if (roamCollide === null || !session?.net?.solid) break;
      roamCollide = roamCollide === 'on' ? 'off' : roamCollide === 'off' ? 'none' : 'on';     // off, players, everything, off
      session.net.solid(roamCollide);
      m.querySelector<HTMLButtonElement>('[data-act="solid"]')!.textContent = ghostLabel();
      // and in settings.ini for the next game, as Online, Ghost mode would
      const text = putKey(settingsText ?? TEMPLATE, 'roam_collide', roamCollide).text;
      try { await writeFile(SAVE, 'settings.ini', text); settingsText = text; void settings.refresh(); }
      catch (e) { log.add(`[app] settings.ini not written: ${e}`); }
      log.add(`[app] roam_collide = ${roamCollide} (the page menu)`);
      break;
    }
    case 'crews': openCrews(); break;
    case 'friends': openFriends(); break;
    case 'friend': m.close(); friendPrompt.menuItem()?.act(); break;
    case 'radio-folder': case 'radio': await radioClick(act); showRadio(m); break;
    case 'playlist': openPlaylist(); break;
    case 'help': m.close(); openHelp(); break;
    case 'offline':
      m.close();
      if (!inLine || !session?.net?.offline) break;
      session.net.offline();
      toast(t('Playing offline: you left the line until the next game.'), 5000);
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
function choiceBox(title: string, body: Node | string, ok: string, cancel: string, danger = false, about = '', cancelDanger = false): Promise<string> {
  const d = $<HTMLDialogElement>('#confirm');
  d.dataset.about = about;            // 'quit': closed when the game ends (endStage)
  $('#confirm-title').textContent = title;
  const b = $('#confirm-body'); b.replaceChildren(typeof body === 'string' ? Object.assign(document.createElement('p'), { textContent: body }) : body);
  const okb = $<HTMLButtonElement>('#confirm-ok'); okb.textContent = ok; okb.className = danger ? 'btn danger' : 'btn selected';
  okb.hidden = !ok;                                  // '' (cancel '' too): the body's buttons answer (vault-ui.ts askAtPlay)
  okb.parentElement!.hidden = !ok && !cancel;
  const cb = $('#confirm-cancel'); cb.textContent = cancel; cb.hidden = !cancel;     // '': the one button
  cb.className = cancelDanger ? 'btn danger' : 'btn';
  d.returnValue = '';
  d.showModal();
  if (!ok && !cancel) {                              // nothing focused: Enter picks none of them
    d.tabIndex = -1; d.focus();
    d.addEventListener('close', () => d.removeAttribute('tabindex'), { once: true });
  } else if (cancelDanger && !danger) okb.focus();   // the safe one first
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
    moduleLoading = false;
    setProgress(0);
    // the browser's interpreter instead of its compiler: no try again makes that work, the player's setting does
    if (noJit(e)) {
      log.add(`[app] the game module did not load: ${e}; the JavaScript optimizer is off for this site`);
      setCheck(check('module', N('Game module'), 'fail', NO_JIT, 'PAGE-NO-JIT'));
      return;
    }
    const wait = Math.min(30, 2 ** moduleFails);
    log.add(`[app] the game module did not load (attempt ${moduleFails}): ${e}; next try in ${wait} s`);
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
  if (playing) toast(t('A new version is out. Quit when it suits you to get it; your saves stay.'), 10000);
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
function netBack() { setTimeout(() => void checkNewer(), Math.random() * 30_000); friendPrompt.online(); }
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
  live(line, () => n ? (byP ? t('A new version is out: Play reloads the page with it. Your saves and settings stay.')
    : t('A new version is out. Reload the page to play it; your saves and settings stay.')) +
    (log ? ' ' + t('Download the log first if you want to keep it: the reload clears it.') : '')
    : u ? t('Updated to the new version. Your saves and settings stayed.') : '');
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
    live(o, T('This page\'s address has test settings ({names}), which work only on a development server and are ignored here.', { names: given.join(', ') }));
    return;
  }
  const env = Object.entries(ENVIRONMENT).map(([k, v]) => `${k}=${v}`);
  live(o, () => t('Test settings from this page\'s address (development server): save folder "{folder}"', { folder: SAVE }) +
    (env.length ? t('; {vars} (wins over Settings)', { vars: env.join(', ') }) : '') +
    ['fresh', 'reg', 'script', 'artifacts', 'dumpscale', 'away', 'playtime', 'claim'].filter(k => q.has(k)).map(k => `; ${k}`).join('') + '.');
}

export async function boot() {
  // the address's test settings: asked about only when there are some (a player's page asks nothing)
  const asked = TEST_PARAMS.some(k => addr.has(k));
  const dev = asked && await devServer();
  const given = takeParams(dev);
  // this browser's id, put back in a store that lost it before anything reads it (page-id.ts); counted for the day
  // (a test page is not)
  void keepPageId();
  if (!asked) seen('page');
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
      : T('Reloaded after a problem? Download log still has your last game\'s log.'),
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
  // the big panel's tabs (the one open last, or the one the address names: #friends) and About's
  panelTabs($('#settings'), { store: 'nfsu2.page.tab', hash: true });
  panelTabs($('#about'));
  settings = new SettingsPanel($('#settings'), {
    // another language, texture pack or songs count other files as the game data (its pack); a keyphrase sends the file
    saveDir: SAVE, environment: ENVIRONMENT, defaults: DEVICE, onSaved: text => {
      const was = packsKey(); settingsText = text;
      if (packsKey() !== was) void saves.refresh();
      vault?.changed(); players?.now();
      applyLanguage();
    },
    onLoaded: applyLanguage,
    // the tab that owns the saves is the one whose Play reads the file: another tab only looks
    readOnly: () => !instance.owner, say: pageToast, confirm: confirmBox,
    nameRow: (row, input, ctl) => nameRow.attach(row, input, ctl),
  }, $('#online'));
  // the language picker at the top: the same setting as the Game tab's Language
  const pick = $<HTMLSelectElement>('#lang-pick');
  pick.replaceChildren(...LANGUAGES.map(l => Object.assign(document.createElement('option'), { value: l, textContent: NATIVE[l], lang: LOCALE[l] })));
  pick.onchange = () => void settings.set('language', pick.value);
  // Donate at the top: a popup with the Ko-fi link, closed as Ko-fi opens in its own tab
  $('#donate-btn').onclick = () => $<HTMLDialogElement>('#donate').showModal();
  $('#donate-go').onclick = () => $<HTMLDialogElement>('#donate').close();
  // Changelog at the top: CHANGELOG.md's releases in a popup
  changelog($('#changelog-btn'), $<HTMLDialogElement>('#changelog'), $('#changelog-body'));
  fitTop(); onLanguage(fitTop); window.addEventListener('resize', fitTop); void document.fonts.ready.then(fitTop);
  // who plays on this server now (Playing now): only while no game runs here; with free roam on in a room, how many
  // are in that room (on the Room row's line), and in crews (free roam on or not: the row shows them either way), how
  // many are in each (on the Crews row's lines)
  playersPanel = new PlayersPanel($('#players'));
  players = watchPlayers((c, room, crews) => { playersPanel.show(c); settings.setRoomLine(c ? () => roomLine(c, room) : () => ''); settings.setCrewCounts(crews); },
    () => !session && !starting, setReach,
    () => { const e = resolved().env; return e.NFSU2_ROAM === '1' ? e.NFSU2_ROAM_ROOM ?? '' : ''; },
    () => crewList(resolved().env.NFSU2_ROAM_CREW ?? '').map(roomHash));
  window.addEventListener('online', () => players?.now());
  window.addEventListener('resize', placeNotices);
  window.addEventListener('resize', placeLobby);
  download = new Predownload(s => saves.showData(s), logLine, () => language(), () => packs());
  saves = new SavesPanel($('#saves'), $('#game-data'), {
    saveDir: SAVE, busy: () => !!session || starting || !instance.owner, confirm: confirmBox,
    onRestored: () => { void loadSettings(); void settings.refresh(); }, download, onChanged: () => vault.changedSaves(),
  });
  // keyphrase saves (on a server that keeps them): synced by the tab that owns the saves, once its settings are loaded
  // the claimed names: asked with the keyphrase's token (claim.ts); the line under Your name (claim-ui.ts)
  claims = new NameClaims(() => vault ? vault.token() : Promise.resolve(null));
  nameRow = new NameRow({
    claims, seed: pageId, linked: () => !!vault?.linked(), enable: enableCloud, showPassphrase,
    profileName: async () => (await newestProfile())?.name ?? null,
    save: n => settings.putName(n), writable: () => instance.owner,
    // (a name claimed, or another passphrase's: the Friends & crews tab's list at once, not at its next minute)
    kept: n => { vault?.showClaim(n); if (claimedName !== undefined && n !== claimedName) void friendsPanel.refresh(true); claimedName = n; },
    say: pageToast, log: logLine,
  });
  vault = new VaultPanel($('#vault'), {
    saveDir: SAVE, owner: () => instance.owner, playing: () => !!session || savesLocked, busy: () => !!session || starting || !instance.owner,
    confirm: confirmBox, choose: (title, body, ok, cancel, danger, cancelDanger) => choiceBox(title, body, ok, cancel, danger, '', cancelDanger),
    onRestored: () => { void loadSettings(); void settings.refresh(); void saves.refresh(); }, log: logLine,
    // another passphrase here (or none): the line under Your name asks again with it, and the friends
    onLink: () => { void nameRow.ask(); void friendsPanel.refresh(true); },
  });
  friendsPanel = new FriendsPanel($('#friends'), {
    api: new FriendsApi(() => testClaim() ? Promise.resolve(testClaim()) : vault ? vault.token() : Promise.resolve(null)),
    join: room => void offerInvite({ room }), confirm: (title, body, ok) => confirmBox(title, body, ok),
    idle: () => !session && !starting, log: logLine, store: msgStore, chat: name => chatDialog.open(name),
  });
  chatDialog = new ChatDialog(msgStore, { send: (name, body) => friendsPanel.send(name, body), log: logLine });
  $<HTMLButtonElement>('#play').onclick = () => void play();
  $<HTMLButtonElement>('#retry').onclick = () => void loadModule();
  $<HTMLButtonElement>('#reload').onclick = () => void reload();
  $<HTMLButtonElement>('#log-btn').onclick = downloadLog;
  $('#menu').addEventListener('click', e => { const a = (e.target as HTMLElement).closest<HTMLButtonElement>('[data-act]'); if (a) void menuAction(a.dataset.act!); });
  $('#playlist-body').addEventListener('change', setTrackMode);
  $('#playlist-shuffle').addEventListener('change', setShuffle);
  $('#menu').addEventListener('close', () => {
    for (const id of ['#crews-pop', '#friends-pop']) { const d = $<HTMLDialogElement>(id); if (d.open) d.close(); }   // the menu's Crews and Friends go with it
    $('#stage').classList.remove('cursor'); $('#stage').querySelector('canvas')?.focus();
  });
  // the menu's Crews popup: Done closes it; back on the menu's Crews button, with its count as it is now
  $('#crews-pop').addEventListener('click', e => { if ((e.target as HTMLElement).closest('[data-act="crews-done"]')) $<HTMLDialogElement>('#crews-pop').close(); });
  $('#crews-pop').addEventListener('close', () => {
    const b = $<HTMLButtonElement>('#menu [data-act="crews"]');
    b.textContent = crewsLabel();
    if ($<HTMLDialogElement>('#menu').open) b.focus();
  });
  // ...and its Friends popup
  $('#friends-pop').addEventListener('click', e => { if ((e.target as HTMLElement).closest('[data-act="friends-done"]')) $<HTMLDialogElement>('#friends-pop').close(); });
  $('#friends-pop').addEventListener('close', () => { if ($<HTMLDialogElement>('#menu').open) $<HTMLButtonElement>('#menu [data-act="friends"]').focus(); });

  // F7 / F8 / F9 belong to the page while the game runs (the game binds none
  // of them, Chrome reserves none); caught first, so the game never sees them,
  // nor the browser F7 (caret browsing).  F7: the next song (RadioLink.skip,
  // runtime/radio.c).  F12 too: photo mode in the world, its picture while it
  // is on, else the page's screenshot (not the game's, whose copy of the frame
  // comes out black in this port, nor the browser's developer tools).
  window.addEventListener('keydown', e => {
    if (session && (e.code === 'F7' || e.code === 'F8' || e.code === 'F9' || e.code === 'F12')) {
      e.preventDefault(); e.stopImmediatePropagation();
      if (e.repeat) return;
      if (e.code === 'F7') radioPort()?.skip();
      else if (e.code === 'F8') toast(fpsLabel(fps.cycle()), 1500); else if (e.code === 'F9') openMenu(); else photo.f12();
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
    friendsPanel.render();
    helpSheet = null; $('#help-body').replaceChildren();
    syncPicker();
  });
  const stageMenu = $<HTMLButtonElement>('#stage-menu');
  stageMenu.onpointerdown = e => e.preventDefault();     // the canvas keeps the focus (and with it the keyboard)
  stageMenu.onclick = () => { if (session) openMenu(); };
  $('#stage').addEventListener('pointerdown', e => { if (e.pointerType === 'touch') stageMenu.hidden = false; }, { capture: true });
  quickChat = new QuickChat($('#stage'), {
    line: chatLine, lobby: (name, text) => lobbyPanel.say(name, text), place: placeNotices, log: logLine,
    message: { on: () => friendMessages?.enabled ?? false, open: () => friendMessages.pickByTouch() },
  });
  touch = new TouchControls($('#stage'), { toast, log: logLine, chat: () => quickChat.toggle(), chatOn: () => quickChat.enabled,
    settings: () => openHelp('keys') });
  friendPrompt = new FriendPrompt($('#stage'), {
    send: m => session?.net?.friend?.(m) ?? false, room: () => roamRoom ?? '', toast: s => toast(s, 5000), log: logLine,
    notice: friendNotice, menu: menuName, placed: () => { if ($<HTMLDialogElement>('#friends-pop').open) drawPopFriends(); },
  });
  friendMessages = new FriendMessages($('#stage'), msgStore, {
    send: m => session?.net?.friend?.(m) ?? false, driving: () => quickChat.driving(), line: msgLine, hint: msgHint,
    placeNow: placeNotices, focusGame: () => $('#stage').querySelector('canvas')?.focus({ preventScroll: true }), toast: s => toast(s, 5000), log: logLine,
  });
  textField = new TextField($('#stage'), { log: logLine });
  pauseHint = new PauseHint($('#stage'), {
    open: openMenu, log: logLine,
    roam: () => roamCollide !== null && !!session?.net?.solid,
    fullscreen: () => canFullscreen() || !fromHomeScreen(),
  });
  driftPanel = new DriftPanel($('#stage'), { toast, wide: () => hudWide, log: logLine });
  photo = new PhotoMode($('#stage'), { shoot: () => screenshot(true), screenshot: () => void screenshot(), log: logLine });
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
  void vault.start().then(() => friendsPanel.refresh(true));
  void instance.whenOwner.then(async () => {
    // a restore a closed tab left halfway is undone, before anything reads the saves (savefs.ts writeTogether)
    await oneWriter(async () => {
      if (!await settleStaged(SAVE)) log.add('[app] a restore that stopped halfway could not be undone yet: tried again before the next restore');
      await vault.settleKept();
    }).catch(e => log.add(`[app] a restore that stopped halfway could not be undone: ${e}`));
    await loadSettings(); evaluate(); void settings.refresh(); void saves.refresh(); vault.ownerNow(); syncPicker(); });
  // invite links: the one this page was opened with, one pasted into its address later, one another tab took for it
  hearInvites();
  takeInvite();
  window.addEventListener('hashchange', takeInvite);
  void checkGpu(lastGame?.ended?.why === 'GPU-LOST').then(c => { setCheck(c); if (c.code === 'GPU-ADAPTER-AFTER-LOST') recheckGpu(); });
  // Firefox's answer to its storage prompt comes later: the check again then
  void checkStorage().then(({ answer, ...c }) => { setCheck(c); void answer?.then(setCheck); });
  refreshMeasured = measureRefresh(60).then(r => { displayHz = r.hz; log.add(`[app] display refresh ${r.raw.toFixed(1)} Hz -> ${r.hz}`); });

  // no WebAssembly at all (the JavaScript optimizer off): the isolation check says so, once
  if (crossOriginIsolated) { if (typeof WebAssembly !== 'undefined') await loadModule(); }
  else setCheck(check('module', N('Game module'), 'fail', N('Not loaded: the page is not isolated (see above).'), 'MODULE-NOT-ISOLATED'));
  evaluate();
  (window as any).__nfsu2 = {
    get phase() { return phase; }, get lastExit() { return lastExit; }, get fps() { return { game: fps.gameFps, page: fps.pageFps, source: $('#fps').dataset.source }; },
    get session() { return session; }, log, save: SAVE, download: () => download.status, offline: () => offline, displayHz: () => displayHz, retryModule: () => loadModule(),
    newer: () => newer, tried: () => tried, touch: () => touch.state(), textField: () => textField.state(), renderCheck: () => renderCheck?.state() ?? null,
    quickChat: () => quickChat.state(), quickChatStick: (x: number, y: number) => quickChat.stick(x, y),
    quickChatClick: () => quickChat.click(),
    messages: () => friendMessages.state(),
    env: () => resolved().env, settingsText: () => settingsText, checks: () => [...checks.values()],
    fpsMode: (m?: FpsMode) => { if (m && FPS_MODES.includes(m)) fps.set(m); return fps.mode; },
    quitTimes: { forcedCloseMaxMs: FORCED_CLOSE_MAX_MS, stopAfterMs: STOP_AFTER_MS },
  };
}
