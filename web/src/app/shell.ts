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
// While any of them is given, the start page says whether it is in effect.

import registryDefaults from '../../../app/registry-defaults.txt';
import { checkBrowser, checkGpu, checkIsolation, checkStorage, firefoxVersion, fmtBytes, type Check } from './checks';
import { loadGame, serverRev, type Loaded } from './loader';
import { claimInstance } from './instance';
import { LogRing, downloadBlob, stamp } from './logs';
import { enterFullscreen, exitFullscreen, isFullscreen, measureRefresh, settledSize, trackCanvasSize } from './display';
import { checkSetting, HOST_FIXED, pageOnly, resolveEnv, settingsUpgrade, TEMPLATE, type Resolved } from './settings';
import { exists, filesOf, listProfiles, PROFILES_DIR, readText, removeSaveDir, writeFile } from './savefs';
import { turnOnAutosave } from './profile';
import { putKey, SettingsPanel } from './settings-ui';
import { serverAnswers, watchPlayers } from './players';
import { SavesPanel } from './saves-ui';
import { FpsOverlay, FPS_MODES, type FpsMode } from './overlay';
import { GameSession } from './game';
import { AwayCheck, AWAY_LIMITS, type AwayLimits } from './away';
import { moviesKept, Predownload } from './predownload';
import { pageUi, type PageUi } from './page-ui';

type Phase = 'boot' | 'ready' | 'error' | 'waiting' | 'starting' | 'playing' | 'exited';

const $ = <T extends HTMLElement = HTMLElement>(sel: string) => document.querySelector<T>(sel)!;

// the page address's test aids, once boot knows whether the server is a development server
const TEST_PARAMS = ['env', 'save', 'fresh', 'reg', 'script', 'artifacts', 'dumpscale', 'away'];
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
 *  (server.mjs never serves /test/ without --dev).  At most 3 s: a server that does not answer is no dev server. */
async function devServer(): Promise<boolean> {
  try {
    const r = await fetch('/test/dev.json', { cache: 'no-store', signal: AbortSignal.timeout(3000) });
    return r.ok && (await r.json())?.dev === true;
  } catch { return false; }
}

const log = new LogRing(8 << 20);
let phase: Phase = 'boot';
let loaded: Loaded | null = null;
let displayHz = 60;
// NFSU2_DISPLAY_HZ ("fps = auto" follows it): Play waits for the measurement,
// at most 2 s (a hidden tab has no animation frames)
let refreshMeasured: Promise<void> = Promise.resolve();
let session: GameSession | null = null;
let hudWide = true;                      // the running game's HUD is edge-anchored (hud = wide), not 4:3
let away: AwayCheck | null = null;
let lastExit: { code: number; seconds: number } | null = null;
const checks = new Map<string, Check>();
let instance: ReturnType<typeof claimInstance>;
let settingsText: string | null = null;
let starting = false;                    // Play pressed, the session not there yet
let fatal: string | null = null;         // why the running game had to be stopped
let fps: FpsOverlay;
let settings: SettingsPanel;
let players: { now(): void } | null = null;
let saves: SavesPanel;
let download: Predownload;
let ui: PageUi;
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
function setPhase(p: Phase, title: string, sub: string) {
  phase = p;
  document.body.dataset.phase = p;
  $('#status').textContent = title;
  $('#status-sub').textContent = sub;
  updatePlay();
}
function setCheck(c: Check) {
  checks.set(c.id, c);
  renderProblems();
  evaluate();
}
const ORDER = ['browser', 'isolation', 'gpu', 'storage', 'module'];
let problemsShown = '';
/** Only what is wrong (a failed check) or worth knowing (another browser, a GPU without BC); the storage warning goes with the saves. */
function renderProblems() {
  const shown = ORDER.map(id => checks.get(id))
    .filter((c): c is Check => !!c && (c.state === 'fail' || (c.state === 'warn' && (c.id === 'browser' || c.id === 'gpu'))));
  const key = shown.map(c => `${c.id}:${c.state}:${c.detail}`).join('|');
  if (key !== problemsShown) {          // the list is a live region: rebuilt only when it changes
    problemsShown = key;
    $('#problems').replaceChildren(...shown.map(c => {
      const li = document.createElement('li'); li.dataset.id = c.id; li.className = c.state;
      const t = document.createElement('strong'); t.textContent = c.label;
      const p = document.createElement('p'); p.textContent = c.detail;
      li.append(t, p);
      return li;
    }));
  }
  const st = checks.get('storage');
  const note = $('#storage-note');
  note.hidden = st?.state !== 'warn';
  note.textContent = st?.state === 'warn' ? st.detail : '';
}
function blocking(): Check | undefined { return ORDER.map(id => checks.get(id)).find(c => c?.state === 'fail'); }

/** Decide the start page's headline from what is known so far. */
function evaluate() {
  if (phase === 'starting' || phase === 'playing') return;
  const bad = blocking();
  if (bad) {
    setPhase('error', 'Not ready', bad.id === 'module' ? 'The game did not load. It is tried again by itself, or press Retry.' : 'Fix the problem below, then reload this page.');
    return;
  }
  if (!instance.owner) {
    setPhase('waiting', 'Open elsewhere', 'This browser is already running the game in another tab. Switch to that tab, or close it: this page takes over when it does.');
    return;
  }
  const pending = ORDER.map(id => checks.get(id)).find(c => c?.state === 'pending');
  if (!loaded) {
    // downloading (again, after a failure): the headline says so, the progress fills in the line below
    if (pending?.id === 'module' && phase !== 'boot' && phase !== 'exited') setPhase('boot', 'Loading', 'Loading the game.');
    return;
  }
  if (pending) { if (phase !== 'exited') setPhase('boot', 'Loading', `${pending.label}: ${pending.detail}`); return; }
  if (phase === 'exited') { updatePlay(); return; }
  setPhase('ready', 'Ready', offline ? 'Offline: the game runs from this PC, without free roam or Online. Press Play or Enter to start.'
    : 'Press Play or Enter to start.');
}

function updatePlay() {
  const ok = !!loaded && instance.owner && !blocking() && !starting && (phase === 'ready' || phase === 'exited');
  $<HTMLButtonElement>('#play').disabled = !ok;
  $<HTMLButtonElement>('#top-play').disabled = !ok;   // Play in the top bar (page-ui.ts)
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
  return text;
}
function resolved(host: Record<string, string> = {}): Resolved { return resolveEnv(settingsText, ENVIRONMENT, host); }

// ----------------------------------------------------------------- play --
function stageCanvas(): HTMLCanvasElement {
  const stage = $('#stage');
  stage.querySelector('canvas')?.remove();
  const c = document.createElement('canvas');
  c.id = 'game'; c.tabIndex = 0; c.setAttribute('aria-label', 'The game');
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
 * ASK_ROAM false: never asked, and free roam stays off until the player turns it on under Online.
 */
const ASK_ROAM = true;
async function askRoam() {
  const r = resolved();
  if (!ASK_ROAM || r.source.NFSU2_ROAM || TEST_PARAMS.some(k => q.has(k)) || offline || /^(off|0|none)$/i.test(r.env.NFSU2_NET?.trim() ?? '')) return;
  const name = (r.source.NFSU2_ROAM_NAME === 'settings.ini' ? r.env.NFSU2_ROAM_NAME : '') || (await newestProfile())?.name || '';
  const p = (text: string) => Object.assign(document.createElement('p'), { textContent: text });
  const body = document.createElement('div');
  body.append(p('Other players on this server can drive in your career\'s free roam with you. You see their cars with their names over them, they see yours, and you can challenge each other to outruns. Races, traffic and money stay your own.'),
    p('With this on, everyone on this server sees your picked name and whether you\'re driving or in the menus. Players near you also see your car and where it is.'));
  let field: HTMLInputElement | null = null;
  if (name) {
    const row = document.createElement('p'); row.className = 'dlg-field';
    const label = Object.assign(document.createElement('label'), { className: 'set-label', htmlFor: 'roam-ask-name', textContent: 'Your name' });
    const f = field = Object.assign(document.createElement('input'), { type: 'text', id: 'roam-ask-name', value: name, maxLength: 24, autocomplete: 'off', spellcheck: false });
    // a name the setting refuses keeps the question open (Not now and Esc still close it)
    f.oninput = () => { const v = f.value.trim(), c = v ? checkSetting('roam_name', v) : null; f.setCustomValidity(c && !c.ok ? `${c.reason}.` : ''); };
    // Enter in the field turns it on (the form's first button, which Enter would press, is Not now)
    f.onkeydown = e => { if (e.key === 'Enter') { e.preventDefault(); $<HTMLButtonElement>('#confirm-ok').click(); } };
    row.append(label, f);
    body.append(row);
  }
  body.append(p('You can change this any time under Online on the start page.'));
  const answer = confirmBox('Free roam with others?', body, 'Turn on', false, '', 'Not now');
  $<HTMLButtonElement>('#confirm-ok').focus();
  const on = await answer;
  let text = putKey(settingsText ?? TEMPLATE, 'roam', on ? 'on' : 'off').text;
  const typed = field?.value.trim() ?? name;
  if (on && typed !== name) text = putKey(text, 'roam_name', typed || null).text;
  await writeFile(SAVE, 'settings.ini', text);
  settingsText = text;
  void settings.refresh();
  log.add(`[app] free roam with others: ${on ? 'on' : 'off'} (asked at the first Play)${on && typed !== name ? `, name ${typed || '(the profile\'s)'}` : ''}`);
}

async function play() {
  if ($<HTMLButtonElement>('#play').disabled || session || starting) return;
  starting = true; updatePlay();
  try { await startGame(); } finally { starting = false; updatePlay(); }
}

async function startGame() {
  // settings.ini as it is now (another tab or the raw editor may have changed it,
  // and the Mac app reads it at every launch); a short read, well inside the click's
  // user activation, which full screen needs (after the free-roam question, its
  // button's click is the activation)
  await Promise.race([loadSettings().catch(e => log.add(`[app] settings.ini not read again: ${e}`)), new Promise(r => setTimeout(r, 1000))]);
  await askRoam();
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
  setPhase('starting', 'Starting', full ? 'Going full screen and starting the game.' : 'Starting the game.');
  let fsNote = '';
  if (full) {
    const fs = await enterFullscreen(stage);
    if (!fs.ok) fsNote = 'Full screen was not allowed here; F9 has it in the menu.';
    else if (!fs.locked) fsNote = 'Esc leaves full screen in this browser (no keyboard lock).';
  }
  const size = await settledSize(canvas);
  await Promise.race([refreshMeasured, new Promise(r => setTimeout(r, 2000))]);
  canvas.focus();

  await saves.settled();              // a rename or restore the Saves section is writing ends first
  // the save overlay: registry seed (bundle.m app_seed_registry), test inputs
  const host: Record<string, string> = {
    HOME: '/save', NFSU2_DATA: '/data', NFSU2_SAVEDIR: '/save', NFSU2_REGFILE: '/save/registry.txt',
    NFSU2_WIDTH: String(size.w & ~1), NFSU2_HEIGHT: String(size.h & ~1), NFSU2_DISPLAY_HZ: String(displayHz),
  };
  const r = resolved(host);
  // offline mode: the server does not answer.  The movies stream from it, so unless every one of them is in this
  // browser they are off for this game (a movie the game could not read would stop it).
  setReach(await serverAnswers(2000));
  let offlineNote = '';
  if (offline) {
    log.add('[app] offline: the server does not answer, the game runs from this PC (no free roam or Online)');
    if (r.env.NOMOVIES !== '1' && !await moviesKept().catch(() => false)) {
      r.env.NOMOVIES = '1';
      log.add('[app] offline: not every movie is on this PC, so the movies are off for this game');
      offlineNote = 'Offline: the movies are off for this game. ';
    }
  }
  // the log is for sharing when asking for help: it names neither the server (a private server's address) nor
  // more of this browser than its major version and system (the audit's PRIV-5)
  log.add(`=== NFSU2 web, ${new Date().toISOString()}, game ${loaded!.info.rev}, server ${serverKind()} ===`);
  log.add(`[app] save folder: OPFS ${SAVE} (/save); canvas ${size.w}x${size.h} device pixels, display ${displayHz} Hz; ${browserBrief()}`);
  for (const n of r.notes) log.add(`[app] ${n}`);
  const reg = r.env.NFSU2_REGFILE.replace(/^\/save\//, '');
  if (q.get('reg')) {
    await writeFile(SAVE, reg, await (await fetch(q.get('reg')!, { cache: 'no-store' })).text());
    log.add(`[app] registry seeded from ${q.get('reg')} (page address)`);
  } else if (!await exists(SAVE, reg)) {
    if (r.env.NFSU2_SEED_REGISTRY === '0') log.add('[app] no registry yet, the game will run its first-run check');
    else { await writeFile(SAVE, reg, registryDefaults); log.add(`[app] first launch, registry seeded with the built-in defaults (/save/${reg})`); }
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
  const net = !/^(off|0|none)$/i.test(r.env.NFSU2_NET?.trim() ?? '');
  const netName = net ? (r.env.NFSU2_ROAM_NAME?.trim() || 'Player') : null;
  hudWide = !/^(43|4:3)$/i.test(r.env.NFSU2_HUD?.trim() ?? '');
  fatal = null;
  quitAt = null;
  saved = null;
  try {
    // the callbacks are module-level functions, not closures made here: the
    // session's subsystems keep them (some, like a closed AudioContext, a little
    // longer than the game), and a closure from this function would keep its
    // scope, and with it this game, reachable
    session = await GameSession.start({
      module: loaded!.module, info: loaded!.info, saveDir: SAVE, env: r.env, canvas, stage, audio, displayHz, netName, notice, onSaved,
      artifacts: q.get('artifacts') ?? undefined, dumpScale: q.get('dumpscale') ? +q.get('dumpscale')! : undefined,
      toggleFullscreen, log: logLine, onFatal: stopFatal,
    });
  } catch (e) {
    log.add(`[app] the game could not start: ${e}`);
    void audio?.close().catch(() => {});
    endStage();
    setPhase('exited', 'Did not start', `The game could not start: ${e}`);
    $('#log-btn').hidden = $('#report').hidden = false;
    download.resume();
    return;
  }
  setPhase('playing', 'Playing', '');
  document.body.dataset.phase = 'playing';
  away = new AwayCheck(session, awayLimits(), awayLine, logLine);
  fps.memory = session.proc.memory;
  const st = session;
  const waitStatus = setInterval(() => { if (st.status) { fps.reader = st.status; clearInterval(waitStatus); } }, 100);
  fps.set((r.env.NFSU2_FPS_COUNTER as FpsMode) in { off: 1, on: 1, detailed: 1 } ? r.env.NFSU2_FPS_COUNTER as FpsMode : 'off');
  const hint = offlineNote + (fsNote || (full ? 'F9 opens the menu. Hold Esc to leave full screen.' : 'F9 opens the menu: full screen, controls, quit.'));
  if (autosaved.length) toast(`Autosave is now on for ${autosaved.join(', ')}: the game saves by itself (Options, Gameplay, Save). ${hint}`, 9000);
  else toast(hint, 6000);
  void st.exited.then(code => { clearInterval(waitStatus); onExit(st, code); });
  // Chrome may keep the sound off until a click or a key press (its autoplay
  // rule); a controller button does not count, so say what does
  setTimeout(() => {
    if (session !== st || !st.audio || st.audio.state() === 'running' || r.env.NOSOUND === '1') return;
    log.add(`[app] the AudioContext is ${st.audio.state()} after Play`);
    toast('No sound yet: click the game or press a key to turn it on (a controller button cannot).', 8000);
  }, 3000);
}

function logLine(s: string) { log.add(s); }
/** whether the server answered (the start page's /players, or the check at Play): offline mode when not */
function setReach(ok: boolean) {
  if (offline === !ok) return;
  offline = !ok;
  log.add(ok ? '[app] the server answers again' : '[app] the server does not answer: offline');
  if (!ok) settings.setStatus('Not connected to the server: free roam and Online need it.');
  evaluate();
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
/** "Chrome 155 on macOS": enough to tell what ran, nothing more */
function browserBrief(): string {
  const ua = navigator.userAgent;
  const c = /(?:Chrome|Chromium)\/(\d+)/.exec(ua), f = firefoxVersion();
  const os = /CrOS/.test(ua) ? 'ChromeOS' : /Windows/.test(ua) ? 'Windows' : /Mac OS X|Macintosh/.test(ua) ? 'macOS' : /Android/.test(ua) ? 'Android' : /Linux/.test(ua) ? 'Linux' : 'another system';
  return `${f !== null ? `Firefox ${f}` : c ? `Chrome ${c[1]}` : 'another browser'} on ${os}`;
}
/** a subsystem the game cannot do without failed: the running game is stopped */
function stopFatal(what: string) {
  const s = session;
  if (!s || s.ended) return;
  fatal = what;
  log.add(`[app] ${what}: stopping the game`);
  s.terminate();
}

function toggleFullscreen() {
  if (isFullscreen()) exitFullscreen();
  else void enterFullscreen($('#stage')).then(r => { if (!r.ok) toast('Full screen was not allowed.', 4000); });
}

function endStage() {
  away?.stop(); away = null;
  exitFullscreen();
  untrack?.(); untrack = null;
  fps.stop(); fps.reader = null; fps.memory = null;
  const menu = $<HTMLDialogElement>('#menu'); if (menu.open) menu.close();
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
  s.terminate();
  session = null;
  quitAt = null;
  lastExit = { code, seconds };
  endStage();
  players?.now();
  download.resume();
  $('#log-btn').hidden = false;
  // a crash, or a game stopped for want of a subsystem: where to send the log (not a normal close or a Stop)
  $('#report').hidden = !fatal && (code === 0 || code === -1);
  const mins = seconds >= 120 ? `${Math.floor(seconds / 60)} min` : `${seconds.toFixed(0)} s`;
  if (fatal) setPhase('exited', 'Did not start', `The game was stopped: ${fatal}. Download the log to see why; ` +
    (firefoxVersion() !== null ? 'about:support, Graphics, shows what Firefox thinks of the graphics.' : 'chrome://gpu shows what Chrome thinks of the graphics.'));
  else {
    const base = code === 0 ? `The game closed normally after ${mins}.` : code === -1 ? `The game was stopped after ${mins}.`
      : `The game stopped with error code ${code} after ${mins}.`;
    // what is kept: the game's last profile save, never a claim it did not make
    const sv = saved;
    setPhase('exited', code === 0 ? 'See you' : 'Stopped', !sv ? base : code === 0 ? `${base} Progress saved in this browser at ${when(sv.at)} (${sv.name}).`
      : `${base} Your progress up to the last save, at ${when(sv.at)} (${sv.name}), is kept.`);
    if (!sv) void newestProfile().then(p => {
      const sub = $('#status-sub');
      if (phase === 'exited' && !session && sub.textContent === base)
        sub.textContent = `${base} ${p ? `Nothing was saved during this game; the last save is from ${when(p.mtime)} (${p.name}).` : 'No profile is saved in this browser yet.'}`;
    });
  }
  void saves.refresh();               // new or changed profiles
  showNewer();                        // the note on the log, now that there is one
  void checkNewer();
}

/** the game finished writing a file of the save folder: a profile is the player's progress */
function onSaved(path: string) {
  const p = path.split('/');
  if (p.length !== 3 || p[0] !== PROFILES_DIR || p[2].toLowerCase() !== p[1].toLowerCase()) return;
  saved = { name: p[1], at: Date.now(), count: (saved?.count ?? 0) + 1 };
  log.add(`[app] the game saved profile ${p[1]} (${saved.count} this game)`);
  if (session) toast(`Progress saved (${p[1]}).`, 2500);
  if ($<HTMLDialogElement>('#menu').open) $('#menu-save').textContent = `Last saved at ${when(saved.at)} (${saved.name}).`;
}
/** the newest profile in this browser (listProfiles sorts by time), or null */
async function newestProfile() { return (await listProfiles(SAVE).catch(() => []))[0] ?? null; }
/** a save's time: the clock today, the day too before today */
function when(t: number): string {
  const d = new Date(t), time = d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  return d.toDateString() === new Date().toDateString() ? time : `${d.toLocaleDateString([], { day: 'numeric', month: 'short' })}, ${time}`;
}

/** a short message at the bottom of the start page (a status region: read out too) */
let pageToastTimer: ReturnType<typeof setTimeout> | undefined;
function pageToast(msg: string) {
  const t = $('#page-toast');
  t.textContent = msg;
  clearTimeout(pageToastTimer);
  pageToastTimer = setTimeout(() => { t.textContent = ''; }, 5000);
}

/** the controls sheet, in the help dialog (the page's ? button, and F9, Controls, while the game runs) */
function openHelp() { ui.openHelp('help-body'); }

/** free roam: a friend joined, left, went into a race (who: the friend's name the text starts with); the server
 *  full; four at most, each for 5 s (also in the log).  Friends' lines are only logged for now: with the crowds
 *  on the server they came too often */
const FRIEND_NOTICES = false;
function notice(text: string, who?: string) {
  log.add(`[notice] ${text}`);
  if (who && !FRIEND_NOTICES) return;
  const box = $('#notices'), line = document.createElement('div');
  if (who && text.startsWith(who)) { const b = document.createElement('b'); b.textContent = who; line.append(b, text.slice(who.length)); }
  else line.textContent = text;
  placeNotices();
  box.append(line);
  while (box.childElementCount > 4) box.firstElementChild!.remove();
  setTimeout(() => line.remove(), 5000);
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
  const s = $('#notices').style;
  s.left = `${v.x + v.w / 2 - (301 + out) * u}px`;
  s.top = `${v.y + v.h / 2 - 72 * u}px`;
  s.setProperty('--u', `${u}px`);
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

function toast(msg: string, ms: number) {
  const t = $('#toast');
  t.textContent = msg; t.hidden = false;
  clearTimeout((toast as any).timer);
  (toast as any).timer = setTimeout(() => { t.hidden = true; }, ms);
}

// ----------------------------------------------------------- page menu --
function openMenu() {
  const m = $<HTMLDialogElement>('#menu');
  if (m.open) { m.close(); return; }
  m.querySelector<HTMLButtonElement>('[data-act="fullscreen"]')!.textContent = isFullscreen() ? 'Leave full screen' : 'Full screen';
  m.querySelector<HTMLButtonElement>('[data-act="fps"]')!.textContent = `Frame-rate counter: ${fps.mode}`;
  m.querySelector<HTMLButtonElement>('[data-act="quit"]')!.textContent = quitLabel();
  const line = $('#menu-save');
  if (saved) line.textContent = `Last saved at ${when(saved.at)} (${saved.name}).`;
  else {
    line.textContent = 'Nothing saved during this game yet.';
    void newestProfile().then(p => {
      if (!saved && m.open) line.textContent = p ? `Nothing saved during this game yet. Last save: ${when(p.mtime)} (${p.name}).` : 'No profile is saved in this browser yet.';
    });
  }
  $('#stage').classList.add('cursor');
  m.showModal();
}
async function menuAction(act: string) {
  const m = $<HTMLDialogElement>('#menu');
  switch (act) {
    case 'resume': m.close(); break;
    case 'fullscreen':
      m.close();
      toggleFullscreen();
      break;
    case 'fps': fps.cycle(); m.querySelector<HTMLButtonElement>('[data-act="fps"]')!.textContent = `Frame-rate counter: ${fps.mode}`; break;
    case 'help': m.close(); openHelp(); break;
    case 'log': downloadLog(); break;
    case 'quit': {
      m.close();
      const s = session;
      if (!s) break;
      const stage = quitStage(s);
      if (stage === 0) {
        if (s.requestQuit()) { quitAt = { s, t: performance.now(), forced: false }; toast('Quitting: the game saves and closes.', 5000); break; }
      } else if (stage === 1) {
        // the game's own second-request rule: open save files are finished, then it exits
        if (await confirmBox('Close the game now?', 'The game has not closed yet (a loading screen ignores Quit). Closing now finishes any save in progress first.', 'Close now', true, 'quit') && session === s) {
          if (s.requestQuit()) { quitAt = { s, t: performance.now(), forced: true }; toast('Closing the game.', 4000); break; }
        } else break;
      }
      // the game cannot be asked (any more), or it did not close when asked twice
      if (await confirmBox('Stop the game now?', s.canQuit()
          ? 'The game did not close when asked. Stopping it ends it at once: progress since it last saved is lost.'
          : 'The game cannot be asked to quit, so it is stopped at once. Progress since it last saved is lost.', 'Stop the game', true, 'quit') && session === s) s.terminate();
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
  if (!session) return 'Quit game';
  return ['Quit game', 'Close the game now', 'Stop the game'][quitStage(session)];
}

// ------------------------------------------------------------- helpers --
function confirmBox(title: string, body: Node | string, ok: string, danger = false, about = '', cancel = 'Cancel'): Promise<boolean> {
  const d = $<HTMLDialogElement>('#confirm');
  d.dataset.about = about;            // 'quit': closed when the game ends (endStage)
  $('#confirm-title').textContent = title;
  const b = $('#confirm-body'); b.replaceChildren(typeof body === 'string' ? Object.assign(document.createElement('p'), { textContent: body }) : body);
  const okb = $<HTMLButtonElement>('#confirm-ok'); okb.textContent = ok; okb.className = danger ? 'btn danger' : 'btn selected';
  $('#confirm-cancel').textContent = cancel;
  d.returnValue = '';
  d.showModal();
  return new Promise(res => d.addEventListener('close', () => res(d.returnValue === 'ok'), { once: true }));
}
function downloadLog() { downloadBlob(log.text(), `nfsu2-${stamp()}.log`, 'text/plain'); }

// --------------------------------------------------------- game module --
// A failed download (Wi-Fi drop, the server restarting, the module not built
// yet) is its own check, tried again with back-off (2, 4, 8 ... 30 s), at the
// next answer from the server, or with Retry.
let moduleLoading = false, moduleFails = 0, moduleTimer: ReturnType<typeof setTimeout> | undefined;
async function loadModule() {
  clearTimeout(moduleTimer);
  if (moduleLoading || loaded) return;
  moduleLoading = true;
  setCheck({ id: 'module', label: 'Game module', state: 'pending', detail: moduleFails ? `Loading again (attempt ${moduleFails + 1}).` : 'Loading.' });
  try {
    loaded = await loadGame((got, total) => {
      setProgress(total ? got / total : 0);
      if (phase === 'boot') $('#status-sub').textContent = `Loading the game: ${fmtBytes(got)} of ${fmtBytes(total)}.`;
    });
    setProgress(1);
    log.add(`[app] game module ${loaded.info.rev}: ${fmtBytes(loaded.info.size)} in ${loaded.ms.toFixed(0)} ms, ${loaded.transferred} bytes transferred${moduleFails ? `, after ${moduleFails} failed attempts` : ''}`);
    moduleFails = 0;
    moduleLoading = false;
    setCheck({ id: 'module', label: `Game module ${loaded.info.rev}`, state: 'ok', detail: `${fmtBytes(loaded.info.size)}, ready.` });
    keepForOffline();
  } catch (e) {
    moduleFails++;
    const wait = Math.min(30, 2 ** moduleFails);
    log.add(`[app] the game module did not load (attempt ${moduleFails}): ${e}; next try in ${wait} s`);
    moduleLoading = false;
    setProgress(0);
    setCheck({ id: 'module', label: 'Game module', state: 'fail', detail: `The game did not load (${e}). Trying again in ${wait} s, or press Retry.` });
    moduleTimer = setTimeout(() => void loadModule(), wait * 1000);
  }
}

// ------------------------------------------------------- newer version --
// The module stays loaded while the page is open, through every game it plays,
// so a page left open plays the version it loaded first even once the server
// has a newer one (a player's log of 2026-10-20: one page, open from 07:50 to
// 11:10 UTC, ran four games on it).  After each game, and when the tab is
// shown again, the page asks the server which version it has; when that is
// not the loaded one, it says so and offers Reload, which brings the page and
// the module of the new version together.
let newer: string | null = null;
async function checkNewer() {
  if (!loaded || newer || session || starting || offline) return;
  const rev = await serverRev(5000);
  if (!rev || rev === loaded.info.rev || newer) return;
  newer = rev;
  log.add(`[app] the server has game ${rev} now, this page ${loaded.info.rev}: Reload offered`);
  showNewer();
}
/** the offer; once a game has run here, with a word on its log (only in this page: a reload clears it) */
function showNewer() {
  const line = $('#newer');
  line.textContent = !newer ? '' : 'A new version of the game is on the server. Reload this page to play it: your saves and settings stay.' +
    (lastExit ? ' Download the log first if you want to keep it: the reload clears it.' : '');
  line.hidden = $('#reload').hidden = !newer;
}
async function reload() {
  await saves.settled();              // a rename or restore the Saves section is writing ends first
  location.reload();
}

// ----------------------------------------------------------------- boot --
/** the start page says when the address carries test settings: in effect (a development server) or ignored */
function showOverrides(given: string[], dev: boolean) {
  const o = $('#override');
  o.hidden = !given.length;
  if (!given.length) return;
  if (!dev) {
    o.textContent = `This page's address carries test settings (${given.join(', ')}). They work only on a development server and are ignored here: the game runs with your own settings.`;
    return;
  }
  const env = Object.entries(ENVIRONMENT).map(([k, v]) => `${k}=${v}`);
  o.textContent = `Test settings from this page's address (development server): save folder "${SAVE}"` +
    (env.length ? `; ${env.join(', ')} (wins over Settings)` : '') +
    ['fresh', 'reg', 'script', 'artifacts', 'dumpscale', 'away'].filter(k => q.has(k)).map(k => `; ${k}`).join('') + '.';
}

export async function boot() {
  // the address's test settings: asked about only when there are some (a player's page asks nothing)
  const asked = TEST_PARAMS.some(k => addr.has(k));
  const dev = asked && await devServer();
  const given = takeParams(dev);
  instance = claimInstance(`nfsu2-saves:${SAVE}`);
  ui = pageUi(() => void play(), pageToast);
  fps = new FpsOverlay($('#fps'));
  // made here once: a closure made per game (in startGame) would keep that game's scope reachable
  fps.extra = () => session?.stats() ?? {};
  settings = new SettingsPanel($('#settings'), {
    saveDir: SAVE, environment: ENVIRONMENT, onSaved: t => { settingsText = t; },
    // the tab that owns the saves is the one whose Play reads the file: another tab only looks
    readOnly: () => !instance.owner, say: pageToast,
  }, $('#online'));
  // who plays on this server now (Online): only while no game runs here
  players = watchPlayers((line, short) => { settings.setStatus(line); ui.here(short); }, () => !session && !starting, setReach);
  window.addEventListener('online', () => players?.now());
  window.addEventListener('resize', placeNotices);
  download = new Predownload(s => saves.showData(s), logLine);
  saves = new SavesPanel($('#saves'), $('#game-data'), {
    saveDir: SAVE, busy: () => !!session || starting || !instance.owner, confirm: confirmBox,
    onRestored: () => { void loadSettings(); void settings.refresh(); }, download, onProfiles: n => ui.profiles(n),
  });
  $<HTMLButtonElement>('#play').onclick = () => void play();
  $<HTMLButtonElement>('#retry').onclick = () => void loadModule();
  $<HTMLButtonElement>('#reload').onclick = () => void reload();
  $<HTMLButtonElement>('#log-btn').onclick = downloadLog;
  $('#menu').addEventListener('click', e => { const a = (e.target as HTMLElement).closest<HTMLButtonElement>('[data-act]'); if (a) void menuAction(a.dataset.act!); });
  $('#menu').addEventListener('close', () => { $('#stage').classList.remove('cursor'); $('#stage').querySelector('canvas')?.focus(); });

  // F8 / F9 belong to the page while the game runs (the game binds neither,
  // Chrome reserves neither); caught first, so the game never sees them.
  window.addEventListener('keydown', e => {
    if (session && (e.code === 'F8' || e.code === 'F9')) {
      e.preventDefault(); e.stopImmediatePropagation();
      if (e.repeat) return;
      if (e.code === 'F8') toast(`Frame-rate counter: ${fps.cycle()}`, 1500); else openMenu();
      return;
    }
    // Enter plays only when nothing else has the focus (or Play itself does): a
    // focused link, button or field keeps its own Enter
    const t = e.target as Element | null;
    const onPage = !t || t === document.body || t === document.documentElement || t.id === 'play' || t.id === 'top-play';
    if (!session && e.code === 'Enter' && onPage && !document.querySelector('dialog[open]') && !$<HTMLButtonElement>('#play').disabled) {
      e.preventDefault(); void play();
    }
  }, { capture: true });
  window.addEventListener('beforeunload', e => { if (session) { e.preventDefault(); e.returnValue = ''; } });
  document.addEventListener('fullscreenchange', () => { if (!isFullscreen() && session) toast('Full screen left. F9 has it in the menu.', 3000); });
  document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'visible') void checkNewer(); });

  setCheck(checkBrowser());
  setCheck(checkIsolation());
  setCheck({ id: 'gpu', label: 'WebGPU', state: 'pending', detail: 'Asking for a graphics adapter.' });
  setCheck({ id: 'storage', label: 'Browser storage', state: 'pending', detail: 'Checking.' });
  setCheck({ id: 'module', label: 'Game module', state: 'pending', detail: 'Loading.' });
  setPhase('boot', 'Loading', 'Loading the game.');
  setProgress(0);

  showOverrides(given, dev);
  if (given.length) log.add(dev ? `[app] test settings from the page address: ${given.join(', ')}` : `[app] the page address's test settings (${given.join(', ')}) are ignored: not a development server`);
  if (q.has('fresh') && SAVE !== 'save') { await removeSaveDir(SAVE); log.add(`[app] fresh save folder ${SAVE}`); }
  // the settings and saves show at once (read only until this tab owns the saves), and again once it does
  void settings.refresh();
  void saves.refresh();
  void instance.whenOwner.then(async () => { await loadSettings(); evaluate(); void settings.refresh(); void saves.refresh(); });
  void checkGpu().then(setCheck);
  // Firefox's answer to its storage prompt comes later: the check again then
  void checkStorage().then(({ answer, ...c }) => { setCheck(c); void answer?.then(setCheck); });
  refreshMeasured = measureRefresh(60).then(r => { displayHz = r.hz; log.add(`[app] display refresh ${r.raw.toFixed(1)} Hz -> ${r.hz}`); });

  if (crossOriginIsolated) await loadModule();
  else setCheck({ id: 'module', label: 'Game module', state: 'fail', detail: 'Not loaded: the page is not isolated (see above).' });
  evaluate();
  (window as any).__nfsu2 = {
    get phase() { return phase; }, get lastExit() { return lastExit; }, get fps() { return { game: fps.gameFps, page: fps.pageFps, source: $('#fps').dataset.source }; },
    get session() { return session; }, log, save: SAVE, download: () => download.status, offline: () => offline, displayHz: () => displayHz, retryModule: () => loadModule(),
    newer: () => newer,
    env: () => resolved().env, settingsText: () => settingsText, checks: () => [...checks.values()],
    fpsMode: (m?: FpsMode) => { if (m && FPS_MODES.includes(m)) fps.set(m); return fps.mode; },
    quitTimes: { forcedCloseMaxMs: FORCED_CLOSE_MAX_MS, stopAfterMs: STOP_AFTER_MS },
  };
}
