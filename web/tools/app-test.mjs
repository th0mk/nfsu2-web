// End-to-end check of the player-facing page (web/index.html) in headless
// Chrome, against a running server (started with --dev: the test scripts):
//   node web/tools/app-test.mjs [--base http://localhost:8615] [--out <dir>] [--only a,b,...] [--keep]
//        [--game ug2|mw] (the game's steps; Most Wanted has none yet) [--requests <file>] [--help] (this text and those)
// --base may name a path (http://localhost:8615/u2): the page served under it, every address the steps use too.
// --requests <file>: every request of the run's pages and their workers, one line each (method, address: a path for
// one of the server's own), into the file, and one more step that passes when every one of them stays under --base.
// Steps (each prints PASS/FAIL with evidence; screenshots go to --out):
//   start     the start page renders, every check passes or warns (no problem shown), the module loads
//   settings  the settings on the page edit /save/settings.ini; the file and the derived environment
//             match; a refused value is shown; it survives a reload; Reset restores a line, also one without
//             a row; Reset all settings (after its question) leaves the template and the name; Listen plays
//             the horn picked through
//   play      Play -> the game in the window (not full screen: the default) (null renderer) runs tests/scripts/app_rw_probe.txt:
//             a new profile ROTEST is saved, F12, a Quick Race, then "close" (the game's own
//             quit path) -> exit 0 -> back on the start page; frame-rate overlay (F8) and page
//             menu (F9) while it runs; beforeunload asks while it runs; the log downloads
//   saves     ROTEST is listed; a backup downloads and holds profile, registry and settings
//   restore   the backup restores into an empty save folder (after confirming) and the next
//             launch auto-loads ROTEST from it (tests/scripts/app_load_probe.txt)
//   pcsave    ROTEST's own file, as the PC game keeps it, restores into an empty save folder beside
//             what is there (settings.ini as it was); again, it would replace that one; a copy a save
//             editor changed replaces it after a warning, sealed again, and loads in the game, its bank as edited
//   quit     F9 -> Quit game: the game's own quit path (WM_CLOSE) -> exit 0 -> back on the start page
//   stalllog  F9 -> Download log while the game draws nothing (tests/scripts/app_stall_probe.txt: 10 s): the file
//             says how long ago the last frame was and holds the stall report (before the watchdog's own 15 s)
//   instance  a second tab on the same saves waits, and takes over when the first closes
//   readonly  that second tab's Settings are read-only; Play reads settings.ini again
//   retry     a failed game.wasm download is its own check, retried by itself and by Retry; a new build
//             between /game.json and /game.wasm (409) is no failure: the page asks /game.json again
//   update    the server on the loaded version: nothing said in a game when the tab is shown again; on another
//             one: a word over the game, offered when it ends (with the log's note), offline mode's copy of
//             /game.json untouched, Reload loads the new page and module, and a test page only offers it
//   autoupdate a player's page (no test settings): a new version with nothing on the page to lose reloads it by
//             itself, and the page says so; with a field being typed in it is offered, and Play reloads; a reload
//             that did not bring the version offers it, and is not repeated by itself
//   enterlink Enter in a focused field stays in the field, not the game; Enter on the page plays
//   late      Stop right after Play: whatever attaches after the end is detached at once, and
//             every WebGPU device made for it is destroyed (also when the game ended before it
//             registered its graphics block)
//   quitstages F9 -> Quit game in the first instant (before the game's input is up) still quits;
//             "Close the game now" forces the exit (exit 0); "Stop the game" comes only after the
//             forced close's longest run (a game that does not answer: stopped, code -1)
//   slowgpu   a renderer that comes after the game stopped waiting for it (NFSU2_RENDERER_WAIT_MS)
//             is refused and the game stopped with the reason, not left on a black screen; the page
//             points to #bug-reports on Discord
//   gpulost   every WebGPU call failing in the middle of a game, as in Safari once its graphics process went away
//             (createCommandEncoder throws, device.lost never comes): the game is stopped with GPU-LOST instead of a
//             frozen picture, the page says Play reloads it, and Play does reload it (then plays); the reloaded
//             page's line on the last game says what it held at its last copy (its textures); a reloaded page that
//             gets no adapter at first (Chrome after a hung device) says GPU-ADAPTER-AFTER-LOST, asks again by itself
//             and plays once the adapter is back
//   crash     a trap (exit 134): "Stopped" with the error code, the trap in the log, the pointer to
//             #bug-reports on Discord; a normal close after it takes the pointer away
//   leak     three Play -> Stop rounds with sound on in one tab (the second with free roam on, so
//             the network too): every ended game's memory is collected (WeakRefs + forced gc,
//             Chrome with --js-flags=--expose-gc), so is every AudioContext and GPU device, and
//             measureUserAgentSpecificMemory comes back to where it was before the first Play
//   cache     page -> Ready and Play -> title screen, first time and again (persistent profile)
//   roamask   a player's first Play (no test settings in the address) asks about free roam: not while the
//             server says free roam is busy (/players roamBusy: nothing written, asked at the next Play); without
//             a name no name line, Esc is Not now (roam = off), not asked again; with one, the name to change (a
//             refused one keeps the question open), Enter in it turns free roam on with the new name
//   cloudask  the fifth Play in a browser (four games counted), with a profile there and no cloud saves, asks to turn
//             them on: not at a Play that asked about free roam, not without a profile, not before the fifth; Esc is
//             Not now, not asked again; Turn on makes a keyphrase and shows it (Copy, Download keyphrase, Continue: the
//             game starts, the browser syncs), on the 4 px grid at 1280 and 390 px; the cloud copy deleted after
//   full      a full server (needs one started with --max-peers 1: the step holds that place itself): the first
//             Play's free-roam question says the server is full, and nothing more is asked after it; in line, the
//             place over the game and the page menu's Stop waiting; in once the place frees up; a later Play asks to
//             wait in line or play offline (Esc waits): offline, no network at all; Stop waiting leaves the line
//   page      what the page shows and asks for: no /server-info, /setup or /ca.crt (all 404), every
//             setting has a name and a description, the controls sheet is on the page and in F9's dialog
//   mobile    the start page at phone size
import { chromium } from 'playwright';
import { WebSocket } from 'ws';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { requireDev } from './dev-server.mjs';
import { gameOf, usage, nothingFor, requireGame } from './rig-game.mjs';

const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
// each game's steps (--only), as above: their scripts, screens and saves are Underground 2's
const STEPS_OF = {
  ug2: ['start', 'settings', 'play', 'saves', 'restore', 'pcsave', 'quit', 'stalllog', 'instance', 'readonly', 'retry', 'update',
    'autoupdate', 'enterlink', 'late', 'quitstages', 'slowgpu', 'gpulost', 'crash', 'leak', 'cache', 'roamask', 'cloudask', 'full', 'page', 'mobile'],
};
const NONE = 'none yet: no Most Wanted screens or saves for the steps (M6-30, M6-31)';
const GAME = gameOf(process.argv.slice(2));
if (process.argv.includes('--help')) usage(import.meta.url, GAME, 'Steps (--only)', STEPS_OF[GAME] ? [STEPS_OF[GAME].join(' ')] : [], NONE);
if (!STEPS_OF[GAME]) nothingFor(GAME, NONE);
const BASE = arg('--base', 'http://localhost:8615').replace(/\/+$/, '');
// the page's base path ('/' or '/u2/'): what the steps' own addresses on the server start with
const PATH = new URL(BASE + '/').pathname;
await requireDev(BASE);
await requireGame(BASE, GAME);
const OUT = path.resolve(arg('--out', 'out/web/app-test'));
const ONLY = arg('--only', '') ? new Set(arg('--only').split(',')) : null;
const RUN = `apptest-${Date.now().toString(36)}`;
const SAVE = `${RUN}-a`, SAVE2 = `${RUN}-b`, SAVE3 = `${RUN}-c`;
// the test suite's environment (DESIGN.md section 5); movies stay on/off through settings.ini
const ENV = ['SNDSYNC=0', 'GPUVS=1', 'NFSU2_FNG_LOG=1', 'NFSU2_STATUS_EVERY=600', 'NFSU2_TIMEOUT=400'].map(e => `env=${encodeURIComponent(e)}`).join('&');
fs.mkdirSync(OUT, { recursive: true });

const results = [];
function report(step, ok, evidence) {
  results.push({ step, ok, evidence });
  console.log(`${ok ? 'PASS' : 'FAIL'} ${step}: ${typeof evidence === 'string' ? evidence : JSON.stringify(evidence)}`);
}
const want = s => !ONLY || ONLY.has(s);

// --requests: every request any page (and its workers) of the run makes, and every WebSocket it opens (WS), from each
// context as it is made; the server's own as a path
const REQUESTS = arg('--requests', '');
const requests = [];
const own = a => { const u = new URL(a); return /^(http|ws)s?:$/.test(u.protocol) && u.host === new URL(BASE).host ? u.pathname + u.search : u.href; };
const track = c => {
  if (!REQUESTS) return c;
  c.on('request', r => requests.push(`${r.method()} ${own(r.url())}`));
  const sockets = p => p.on('websocket', w => requests.push(`WS ${own(w.url())}`));
  c.pages().forEach(sockets);
  c.on('page', sockets);
  return c;
};
const browser = await chromium.launch({ channel: 'chrome', headless: !process.argv.includes('--headed'),
  args: ['--enable-unsafe-webgpu', ...(process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [])] });
const newContext = browser.newContext.bind(browser);
browser.newContext = async o => track(await newContext(o));
const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 }, acceptDownloads: true });
const logs = [];
function watch(page, tag) {
  page.on('console', m => { const t = m.text(); logs.push(`${tag} ${t}`); if (process.env.VERBOSE || /PAGEERROR|TRAP|\[app\]/.test(t)) console.log(`  ${tag} ${t}`); });
  page.on('pageerror', e => { logs.push(`${tag} PAGEERROR ${e}`); console.log(`  ${tag} PAGEERROR ${e}`); });
  return page;
}
const shot = (page, name) => page.screenshot({ path: path.join(OUT, name) });
const readOpfs = (page, dir, file) => page.evaluate(async ([d, f]) => {
  try {
    let h = await (await navigator.storage.getDirectory()).getDirectoryHandle(d);
    const parts = f.split('/');
    for (const p of parts.slice(0, -1)) h = await h.getDirectoryHandle(p);
    const file = await (await h.getFileHandle(parts[parts.length - 1])).getFile();
    return { size: file.size, text: file.size < 100000 ? await file.text() : null };
  } catch { return null; }
}, [dir, file]);
const phase = page => page.evaluate(() => window.__nfsu2?.phase);
// every WebGPU device and AudioContext the page makes, with what became of it (late, leak)
const RECORD_GPU_AUDIO = () => {
  const rv = window.__rv = { devices: [], ctxs: [] };
  const rd = GPUAdapter.prototype.requestDevice;
  GPUAdapter.prototype.requestDevice = async function (...a) {
    const d = await rd.apply(this, a);
    const rec = { destroyed: false, lost: null, ref: new WeakRef(d) };
    const ds = d.destroy;
    d.destroy = function () { rec.destroyed = true; return ds.call(this); };
    d.lost.then(i => { rec.lost = i.reason; });
    rv.devices.push(rec);
    return d;
  };
  const AC = window.AudioContext;
  window.AudioContext = class extends AC { constructor(...a) { super(...a); rv.ctxs.push(new WeakRef(this)); } };
};
// the settings show at once, read only until the tab owns the saves
const settingsReady = page => page.waitForFunction(() => !!document.querySelector('#settings .set') && !document.querySelector('#settings').hasAttribute('data-readonly'), null, { timeout: 15000 });
// settings writes are async: each one bumps #settings[data-saved]
const saved = page => page.evaluate(() => +(document.querySelector('#settings').dataset.saved ?? 0));
async function settle(page, before, n = 1) {
  await page.waitForFunction(([b, k]) => +(document.querySelector('#settings').dataset.saved ?? 0) >= b + k, [before, n], { timeout: 10000 });
  for (let last = -1, now = await saved(page); now !== last; ) { last = now; await page.waitForTimeout(250); now = await saved(page); }
}
async function waitPhase(page, want, ms) {
  await page.waitForFunction(w => w.includes(window.__nfsu2?.phase), want, { timeout: ms, polling: 250 });
  return phase(page);
}
async function open(page, query, ms = 60000, until = ['ready', 'error']) {
  await page.goto(`${BASE}/?${query}`);
  await waitPhase(page, until, ms);
  return page;
}
async function playUntilExit(page, label, ms) {
  const t0 = Date.now();
  await page.click('#play');
  await waitPhase(page, ['playing', 'exited'], 60000);
  const fsEl = await page.evaluate(() => document.fullscreenElement?.id ?? null);
  return { t0, fsEl, wait: () => waitPhase(page, ['exited'], ms).then(() => page.evaluate(() => window.__nfsu2.lastExit)) };
}

let backupPath = null;
const page = watch(await ctx.newPage(), 'A');

try {
  // ------------------------------------------------------------ start --
  if (want('start') || want('settings') || want('play') || want('saves') || want('restore') || want('pcsave')) {
    const t0 = Date.now();
    await open(page, `save=${SAVE}&fresh&${ENV}`);
    const ms = Date.now() - t0;
    const checks = await page.evaluate(() => window.__nfsu2.checks().map(c => `${c.id}:${c.state}`));
    const p = await phase(page);
    const loadLine = (await page.evaluate(() => window.__nfsu2.log.tail(50))).find(l => l.includes('game module')) ?? '';
    const tmpl = await readOpfs(page, SAVE, 'settings.ini');
    await page.waitForTimeout(400);                      // the download bar's transition
    await shot(page, 'start.png');
    const problems = await page.locator('#problems li').allTextContents();
    report('start', p === 'ready' && !checks.some(c => c.endsWith(':fail')) && !!tmpl?.text?.startsWith('# NFSU2 settings') &&
      problems.every(t => /tested/.test(t)),            // at most the other-browser note
      { phase: p, checks, problems, pageToReadyMs: ms, loadLine, templateBytes: tmpl?.size });
  }

  // --------------------------------------------------------- settings --
  if (want('settings') || want('play') || want('saves') || want('restore') || want('pcsave')) {
    await settingsReady(page);
    await page.locator('#settings').screenshot({ path: path.join(OUT, 'settings.png') });
    let n0 = await saved(page);
    await page.click('.set[data-key="movies"] .switch'); await settle(page, n0++);          // movies: on -> off
    await page.click('.set[data-key="fps"] .opt:has-text("120")'); await settle(page, n0++);
    await page.click('#settings summary:has-text("More display")');
    await page.click('.set[data-key="fps_counter"] .opt:has-text("Detailed")'); await settle(page, n0++);
    await page.click('.set[data-key="roam"] .switch'); await settle(page, n0++);            // roam on (validated bool)
    await page.fill('#set-roam_name', 'Tester ; a comment'); await page.press('#set-roam_name', 'Tab'); await settle(page, n0++);
    await page.selectOption('#set-horn', 'train'); await settle(page, n0++);
    // Listen plays the horn picked (horn-preview.ts): Stop while it plays, Listen again once it has played
    await page.click('.set[data-key="horn"] .listen');
    const listenState = () => page.evaluate(() => document.querySelector('.set[data-key="horn"] .listen')?.getAttribute('aria-pressed'));
    const listenPlaying = await listenState();
    const listened = await page.waitForFunction(() => document.querySelector('.set[data-key="horn"] .listen')?.getAttribute('aria-pressed') === 'false',
      null, { timeout: 10000 }).then(() => true, () => false);
    await page.click('#online summary:has-text("More online")');
    await page.fill('#set-roam_max_cars', '99'); await page.press('#set-roam_max_cars', 'Tab');   // out of range: refused, not written
    const refusedMsg = await page.locator('.set[data-key="roam_max_cars"] .set-state').textContent();
    await page.click('.set[data-key="roam"] .switch'); await settle(page, n0);              // roam back off
    const file = (await readOpfs(page, SAVE, 'settings.ini')).text;
    const env = await page.evaluate(() => window.__nfsu2.env());
    const lines = file.split('\n').filter(l => /^[a-z_]+ = /.test(l));
    const fileOk = lines.includes('movies = off') && lines.includes('fps = 120') && lines.includes('fps_counter = detailed') &&
      lines.includes('roam_name = Tester ; a comment') && lines.includes('roam = off') && lines.includes('horn = train') && !/^roam_max_cars = /m.test(file);
    const envOk = env.NOMOVIES === '1' && env.NFSU2_FPS === '120' && env.NFSU2_FPS_COUNTER === 'detailed' &&
      env.NFSU2_ROAM_NAME === 'Tester' && env.NFSU2_ROAM === '0' && env.NFSU2_HORN === 'train' && env.SNDSYNC === '0' && !('WINDOW' in env);
    // the raw editor: a refused value is kept in the file but shown as ignored; a setting the panel has no row for
    // (roam_rate) and one the browser ignores (roam_port, the Mac app's) are listed under the groups
    await page.click('#settings-raw summary');
    const ta = page.locator('#settings-text');
    await ta.fill((await ta.inputValue()).replace('#hud = wide', 'hud = ultrawide').replace('#roam_rate = 20', 'roam_rate = 30')
      .replace(/\n?$/, '\nroam_port = 47300\n'));
    n0 = await saved(page);
    await page.click('#settings-text-save'); await settle(page, n0);
    const rawMsg = await page.locator('#settings-text-msg').textContent();
    const hudState = await page.locator('.set[data-key="hud"] .set-state').textContent();
    const otherText = await page.locator('#settings-other').textContent().catch(() => '(none)');
    await page.locator('#settings').screenshot({ path: path.join(OUT, 'settings-edited.png') });
    // survives a reload
    await open(page, `save=${SAVE}&${ENV}`);
    await settingsReady(page);
    const after = await page.evaluate(() => ({
      movies: String(document.querySelector('.set[data-key="movies"] .switch').checked),
      fps: document.querySelector('.set[data-key="fps"] input:checked')?.parentElement.textContent,
      name: document.querySelector('#set-roam_name').value,
    }));
    // Reset puts the template line back
    const moreDisplay = await page.locator('#settings summary:has-text("More display")').textContent();   // fps_counter and hud
    await page.click('#settings summary:has-text("More display")');
    n0 = await saved(page);
    await page.click('.set[data-key="hud"] .text-btn'); await settle(page, n0);
    const file2 = (await readOpfs(page, SAVE, 'settings.ini')).text;
    // Reset on a line without a row (roam_rate): its template line back
    n0 = await saved(page);
    await page.click('#settings-other li:has-text("roam_rate") .text-btn'); await settle(page, n0);
    const file3 = (await readOpfs(page, SAVE, 'settings.ini')).text;
    // Reset all settings: asks, then the template with the name (roam_port, the Mac app's line, goes too)
    await page.click('#settings-reset-all');
    await page.waitForSelector('#confirm[open]');
    const resetAsk = await page.locator('#confirm-title').textContent();
    n0 = await saved(page);
    await page.click('#confirm-ok'); await settle(page, n0);
    const file4 = (await readOpfs(page, SAVE, 'settings.ini')).text;
    const resetAll = { roamRate: /^#roam_rate = 20$/m.test(file3), ask: resetAsk, left: file4.split('\n').filter(l => /^[A-Za-z_]+ = /.test(l)),
      disabled: await page.locator('#settings-reset-all').isDisabled() };
    const resetOk = resetAll.roamRate && resetAsk === 'Reset all settings?' && resetAll.left.join('|') === 'roam_name = Tester' && resetAll.disabled;
    // the steps after this one play with movies off and the detailed counter (play's F8, saves' and restore's
    // settings.ini): set again after the reset
    n0 = await saved(page);
    await page.click('.set[data-key="movies"] .switch'); await settle(page, n0++);
    const more = page.locator('#settings details:has(> summary:has-text("More display"))');
    if (!await more.evaluate(d => d.open)) await page.click('#settings summary:has-text("More display")');
    await page.click('.set[data-key="fps_counter"] .opt:has-text("Detailed")'); await settle(page, n0++);
    report('settings', fileOk && envOk && /not a whole number in 0\.\.32/.test(refusedMsg) && /ignored/.test(rawMsg) &&
      /ultrawide/.test(hudState) && /roam_rate = 30/.test(otherText) && /roam_port = 47300 \(ignored: only used by the Mac app\)/.test(otherText) && after.movies === 'false' && after.fps === '120' && after.name === 'Tester' && /^#hud = wide$/m.test(file2) && /\(2 changed\)$/.test(moreDisplay) &&
      resetOk && listenPlaying === 'true' && listened,
      { setLines: lines, env: { NOMOVIES: env.NOMOVIES, NFSU2_FPS: env.NFSU2_FPS, NFSU2_FPS_COUNTER: env.NFSU2_FPS_COUNTER, NFSU2_ROAM_NAME: env.NFSU2_ROAM_NAME, NFSU2_ROAM: env.NFSU2_ROAM, NFSU2_HORN: env.NFSU2_HORN, WINDOW: env.WINDOW ?? '(unset)' },
        refusedMsg, rawMsg, hudState, otherText, afterReload: after, hudReset: /^#hud = wide$/m.test(file2), resetAll, listen: { playing: listenPlaying, played: listened } });
  }

  // ------------------------------------------------------------- play --
  if (want('play') || want('saves') || want('restore') || want('pcsave')) {
    await open(page, `save=${SAVE}&${ENV}&script=${PATH}tests/scripts/app_rw_probe.txt`);
    const run = await playUntilExit(page, 'rw', 400000);
    // while it runs: overlay, menu, beforeunload
    await page.waitForFunction(() => Number(document.querySelector('#fps')?.dataset.fps) > 0 && document.querySelector('#fps')?.dataset.source === 'game', null, { timeout: 60000 }).catch(() => {});
    await page.waitForTimeout(3000);
    const overlay = await page.evaluate(() => ({ mode: window.__nfsu2.fpsMode(), ...window.__nfsu2.fps, text: document.querySelector('#fps').innerText }));
    await shot(page, 'playing.png');
    await page.keyboard.press('F8');                                 // detailed -> off
    const afterF8 = await page.evaluate(() => window.__nfsu2.fpsMode());
    await page.keyboard.press('F8');                                 // off -> on
    await page.keyboard.press('F9');
    await page.waitForSelector('#menu[open]');
    await shot(page, 'menu.png');
    await page.click('#menu [data-act="resume"]');
    const beforeUnload = await page.evaluate(() => { const e = new Event('beforeunload', { cancelable: true }); window.dispatchEvent(e); return e.defaultPrevented; });
    const exit = await run.wait();
    const secs = ((Date.now() - run.t0) / 1000).toFixed(0);
    await page.waitForTimeout(1000);
    await shot(page, 'exited.png');
    const tail = await page.evaluate(() => window.__nfsu2.log.text());
    fs.writeFileSync(path.join(OUT, 'play-rw.log'), tail);
    const reg = await readOpfs(page, SAVE, 'registry.txt');
    const prof = await readOpfs(page, SAVE, 'NFS Underground 2/ROTEST/ROTEST');
    const dl = page.waitForEvent('download');
    await page.click('#log-btn');
    const logFile = path.join(OUT, (await dl).suggestedFilename());
    await (await dl).saveAs(logFile);
    const logText = fs.readFileSync(logFile, 'utf8');
    const seeded = /first launch, registry seeded/.test(tail);
    const quitPath = /quit requested \(autotest close\)/.test(tail) && /the game exited \(code 0\) after the quit request/.test(tail);
    const noMovies = /settings\.ini:\d+: movies -> NOMOVIES=1/.test(tail);
    const stillAlive = /still running 1200 frames after close/.test(tail);
    report('play', exit?.code === 0 && quitPath && !stillAlive && seeded && noMovies && /NotFirstTime=1/.test(reg?.text ?? '') && !run.fsEl &&
      prof?.size === 54966 && beforeUnload && afterF8 === 'off' && overlay.source === 'game' && overlay.game > 0 &&
      logText.includes('=== NFSU2 web'),
      { exit, seconds: +secs, fullscreenElement: run.fsEl, overlay, afterF8, beforeUnloadPrevented: beforeUnload, registrySeeded: seeded,
        quitThroughGame: quitPath, settingsApplied: noMovies, profileBytes: prof?.size ?? null, registryLines: reg?.text?.split('\n').length,
        logDownload: `${path.basename(logFile)} ${logText.length} bytes` });
  }

  // ------------------------------------------------------------ saves --
  if (want('saves') || want('restore') || want('pcsave')) {
    await page.waitForSelector('#profiles .name', { timeout: 15000 });   // listed again when the game ended
    await page.waitForFunction(() => !/Counting/.test(document.querySelector('#cache-info').textContent));
    const listed = await page.locator('#profiles .name').allTextContents();
    const cacheInfo = await page.locator('#cache-info').textContent();
    await page.locator('#saves').screenshot({ path: path.join(OUT, 'saves.png') });
    const dl = page.waitForEvent('download');
    await page.click('#export');
    backupPath = path.join(OUT, (await dl).suggestedFilename());
    await (await dl).saveAs(backupPath);
    const listing = execFileSync('unzip', ['-l', backupPath], { encoding: 'utf8' });
    execFileSync('unzip', ['-tq', backupPath]);                       // CRCs check out with another unzip
    const ini = execFileSync('unzip', ['-p', backupPath, 'settings.ini'], { encoding: 'utf8' });
    report('saves', listed.includes('ROTEST') && /NFS Underground 2\/ROTEST\/ROTEST/.test(listing) && /registry\.txt/.test(listing) &&
      /^movies = off$/m.test(ini) && !/autotest-script/.test(listing),
      { listed, cacheInfo, backup: path.basename(backupPath), entries: listing.trim().split('\n').slice(3, -2).map(l => l.trim().replace(/\s+/g, ' ')) });
  }

  // ---------------------------------------------------------- restore --
  if (want('restore') && backupPath) {
    const p2 = watch(await ctx.newPage(), 'B');
    await page.close();                                               // free the Web Lock of save A... (B uses another folder anyway)
    await open(p2, `save=${SAVE2}&fresh&${ENV}`);
    await p2.waitForSelector('#profiles li');
    await p2.waitForFunction(() => !document.querySelector('#import').disabled, null, { timeout: 15000 });   // the tab owns the saves
    const before = await p2.locator('#profiles li').allTextContents();
    await p2.setInputFiles('#import', backupPath);
    await p2.waitForSelector('#confirm[open]');
    const confirmText = await p2.locator('#confirm').innerText();
    await shot(p2, 'restore-confirm.png');
    await p2.click('#confirm-ok');
    await p2.waitForFunction(() => /Restored/.test(document.querySelector('#saves-msg').textContent), null, { timeout: 15000 });
    // the message comes first, the list of profiles once the section has read the folder again
    await p2.waitForFunction(() => document.querySelectorAll('#profiles .name').length > 0, null, { timeout: 15000 }).catch(() => {});
    const listed = await p2.locator('#profiles .name').allTextContents();
    const ini = (await readOpfs(p2, SAVE2, 'settings.ini')).text;
    // the restored profile loads in the game
    await open(p2, `save=${SAVE2}&${ENV}&script=${PATH}tests/scripts/app_load_probe.txt`);
    const run = await playUntilExit(p2, 'load', 300000);
    const exit = await run.wait();
    const tail = await p2.evaluate(() => window.__nfsu2.log.text());
    fs.writeFileSync(path.join(OUT, 'play-load.log'), tail);
    const loadedGarage = /\[fng\] f=\d+ top package GarageMain\.fng/.test(tail);
    const noSeed = !/registry seeded/.test(tail);
    report('restore', before.join('').includes('No profiles') && /ROTEST/.test(confirmText) && listed.includes('ROTEST') &&
      /^movies = off$/m.test(ini) && exit?.code === 0 && loadedGarage && noSeed,
      { before, confirm: confirmText.replace(/\s+/g, ' ').slice(0, 300), listed, exit, garageMainReached: loadedGarage, registryFromBackup: noSeed });
    await p2.close();
  }

  // ----------------------------------------------------------- pcsave --
  if (want('pcsave') && backupPath) {
    const prof = execFileSync('unzip', ['-p', backupPath, 'NFS Underground 2/ROTEST/ROTEST']);
    const pcFile = path.join(OUT, 'ROTEST'), editedFile = path.join(OUT, 'ROTEST-edited');
    fs.writeFileSync(pcFile, prof);
    const ed = Buffer.from(prof); ed[0xA16A] ^= 0x40; fs.writeFileSync(editedFile, ed);       // a save editor's money change
    const p3 = watch(await ctx.newPage(), 'C');
    await open(p3, `save=${SAVE3}&fresh&${ENV}`);
    await p3.waitForFunction(() => !document.querySelector('#import').disabled, null, { timeout: 15000 });
    const iniBefore = (await readOpfs(p3, SAVE3, 'settings.ini'))?.text;
    const msg = (re, ms = 15000) => p3.waitForFunction(s => new RegExp(s).test(document.querySelector('#saves-msg').textContent), re, { timeout: ms });
    await p3.setInputFiles('#import', pcFile);
    await p3.waitForSelector('#confirm[open]');
    const confirmText = await p3.locator('#confirm').innerText();
    await shot(p3, 'pcsave-confirm.png');
    await p3.click('#confirm-ok');
    await msg('Added ROTEST');
    await p3.waitForFunction(() => document.querySelectorAll('#profiles .name').length > 0, null, { timeout: 15000 }).catch(() => {});
    const listed = await p3.locator('#profiles .name').allTextContents();
    const written = await readOpfs(p3, SAVE3, 'NFS Underground 2/ROTEST/ROTEST');
    const iniAfter = (await readOpfs(p3, SAVE3, 'settings.ini'))?.text;
    // the same file again would replace it (Cancel: nothing changes); an edited copy replaces it too, with a
    // warning, its checksums made again (only they differ from the edited file)
    await p3.setInputFiles('#import', pcFile);
    await p3.waitForSelector('#confirm[open]');
    const againText = await p3.locator('#confirm').innerText();
    await p3.click('#confirm-cancel');
    await msg('Nothing changed');
    await p3.setInputFiles('#import', editedFile);
    await p3.waitForSelector('#confirm[open]');
    const editedText = await p3.locator('#confirm').innerText();
    await shot(p3, 'pcsave-edited.png');
    await p3.click('#confirm-ok');
    await msg('Added ROTEST from ROTEST-edited');
    const sealed = Buffer.from(await p3.evaluate(async ([d, f]) => {
      let h = await (await navigator.storage.getDirectory()).getDirectoryHandle(d);
      const parts = f.split('/');
      for (const p of parts.slice(0, -1)) h = await h.getDirectoryHandle(p);
      return [...new Uint8Array(await (await (await h.getFileHandle(parts[parts.length - 1])).getFile()).arrayBuffer())];
    }, [SAVE3, 'NFS Underground 2/ROTEST/ROTEST']));
    const resealed = [];
    for (let i = 0; i < Math.max(sealed.length, ed.length); i++) if (sealed[i] !== ed[i]) resealed.push(i);
    // the edited one loads in the game, with fresh game options (no registry.txt came with it), its bank as edited
    await open(p3, `save=${SAVE3}&${ENV}&script=${PATH}tests/scripts/app_load_probe.txt`);
    const run = await playUntilExit(p3, 'pcload', 300000);
    const exit = await run.wait();
    const tail = await p3.evaluate(() => window.__nfsu2.log.text());
    fs.writeFileSync(path.join(OUT, 'play-pcload.log'), tail);
    const loadedGarage = /\[fng\] f=\d+ top package GarageMain\.fng/.test(tail);
    const bank = { before: prof.readUInt32LE(0xA16A), edited: ed.readUInt32LE(0xA16A), game: Number(/\[career\] f=\d+ stage \d+ bank \$(\d+)/.exec(tail)?.[1]) };
    report('pcsave', /^Add this profile\?/m.test(confirmText) && /Profiles: ROTEST/.test(confirmText) && !/Replaces/.test(confirmText) &&
      listed.includes('ROTEST') && written?.size === prof.length && iniAfter === iniBefore && /Replaces the profile ROTEST/.test(againText) &&
      /Replaces the profile ROTEST/.test(editedText) && /ROTEST was changed outside the game, by a save editor/.test(editedText) &&
      sealed.length === ed.length && resealed.length > 0 && resealed.every(i => i >= 16 && i < 28) &&
      exit?.code === 0 && loadedGarage && bank.game === bank.edited,
      { confirm: confirmText.replace(/\s+/g, ' ').slice(0, 300), again: againText.replace(/\s+/g, ' ').slice(0, 300), listed,
        profileBytes: written?.size, settingsKept: iniAfter === iniBefore, edited: editedText.replace(/\s+/g, ' ').slice(0, 400),
        resealed, exit, garageMainReached: loadedGarage, bank });
    await p3.close();
  }

  // --------------------------------------------------------- instance --
  if (want('instance')) {
    const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const a = watch(await c2.newPage(), 'I1'), b = watch(await c2.newPage(), 'I2');
    await open(a, `save=${RUN}-i&fresh`);
    await open(b, `save=${RUN}-i`, 60000, ['waiting', 'ready', 'error']);
    const pb = await phase(b);
    const playDisabled = await b.locator('#play').isDisabled();
    await shot(b, 'second-tab.png');
    await a.close();
    const taken = await waitPhase(b, ['ready'], 15000).catch(() => phase(b));
    report('instance', pb === 'waiting' && playDisabled && taken === 'ready', { secondTab: pb, playDisabled, afterFirstClosed: taken });
    await c2.close();
  }

  // ------------------------------------------------------------- quit --
  // F9 -> Quit game: the input layer's requestQuit (WM_CLOSE, the Mac's Cmd-Q) in the
  // main menu -> the game closes itself (exit 0) -> back on the start page
  if (want('quit') || want('stop')) {
    const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const st = watch(await c2.newPage(), 'T');
    await open(st, `save=${RUN}-t&fresh&env=NOMOVIES%3D1&env=NFSU2_FNG_LOG%3D1`);
    await st.click('#play');
    await waitPhase(st, ['playing'], 60000);
    await st.waitForFunction(() => window.__nfsu2.log.tail(300).some(l => /top package UG_LS_Splash\.fng/.test(l)), null, { timeout: 120000 });
    await st.waitForTimeout(1500);
    await st.keyboard.press('F9');
    await st.waitForSelector('#menu[open]');
    const label = await st.locator('#menu [data-act="quit"]').textContent();
    await shot(st, 'quit-menu.png');
    await st.click('#menu [data-act="quit"]');
    const p = await waitPhase(st, ['exited'], 60000);
    const exit = await st.evaluate(() => window.__nfsu2.lastExit);
    const head = await st.locator('#status').textContent(), sub = await st.locator('#status-sub').textContent();
    const fsAfter = await st.evaluate(() => !!document.fullscreenElement);
    const tail = (await st.evaluate(() => window.__nfsu2.log.tail(400))).join('\n');
    const viaGame = /quit requested \(page quit request\)/.test(tail) && /the game exited \(code 0\)/.test(tail);
    const bugReport = await st.locator('#report').isVisible();
    await shot(st, 'quit-exited.png');
    report('quit', label === 'Quit game' && p === 'exited' && exit?.code === 0 && viaGame && !fsAfter && !bugReport && !(await st.locator('#play').isDisabled()),
      { menuLabel: label, exit, quitThroughGame: viaGame, headline: head, sub, fullscreenAfter: fsAfter, bugReport });
    await c2.close();
  }

  // F9 -> Download log while the game is stuck (its thread blocked with the GIL held): the game's input
  // thread still answers, so the file has the stall report a player who downloads early used to miss
  if (want('stalllog')) {
    const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 }, acceptDownloads: true });
    const st = watch(await c2.newPage(), 'L');
    await open(st, `save=${RUN}-l&fresh&env=NOMOVIES%3D1&env=NFSU2_FNG_LOG%3D1&script=${PATH}tests/scripts/app_stall_probe.txt`);
    await st.click('#play');
    await waitPhase(st, ['playing'], 60000);
    await st.waitForFunction(() => window.__nfsu2.log.tail(300).some(l => /stall 10 s \(test-only\)/.test(l)), null, { timeout: 120000 });
    await st.waitForTimeout(4000);
    await st.keyboard.press('F9');
    await st.waitForSelector('#menu[open]');
    const dl = st.waitForEvent('download');
    const t0 = Date.now();
    await st.click('#menu [data-act="log"]');
    const file = path.join(OUT, (await dl).suggestedFilename());
    const ms = Date.now() - t0;
    await (await dl).saveAs(file);
    const text = fs.readFileSync(file, 'utf8');
    await st.click('#menu [data-act="resume"]').catch(() => {});
    const ago = +(/\[watchdog\] log downloaded: the last frame ([\d.]+) s ago/.exec(text)?.[1] ?? -1);
    const stallReport = /\[watchdog\] no Present for [\d.]+ s/.test(text) && /\[watchdog\] end of report/.test(text) && /holds the GIL/.test(text);
    const ready = text.includes('[watchdog] log download ready');
    const p = await waitPhase(st, ['exited'], 120000);
    const exit = await st.evaluate(() => window.__nfsu2.lastExit);
    report('stalllog', ago >= 3 && ago < 10 && stallReport && ready && ms < 2500 && p === 'exited' && exit?.code === 0,
      { lastFrameAgo: ago, stallReport, ready, downloadMs: ms, exit });
    await c2.close();
  }

  // --------------------------------------------------------- readonly --
  // a second tab on the same saves shows Settings read-only; the owning tab's Play
  // reads settings.ini again (here changed behind the panel's back)
  if (want('readonly')) {
    const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const a = watch(await c2.newPage(), 'O1'), b = watch(await c2.newPage(), 'O2');
    await open(a, `save=${RUN}-o&fresh&env=NOMOVIES%3D1`);
    await open(b, `save=${RUN}-o`, 60000, ['waiting', 'ready', 'error']);
    await b.waitForSelector('#settings[data-readonly] .set');
    const before = (await readOpfs(b, `${RUN}-o`, 'settings.ini')).text;
    const ro = await b.evaluate(() => ({ note: document.querySelector('#settings-note').textContent,
      disabled: [...document.querySelectorAll('#settings-body button, #settings-body input')].every(e => e.disabled),
      textareaReadOnly: document.querySelector('#settings-text').readOnly, saveDisabled: document.querySelector('#settings-text-save').disabled }));
    await b.click('.set[data-key="movies"] .switch', { force: true }).catch(() => {});
    await b.waitForTimeout(500);
    const after = (await readOpfs(b, `${RUN}-o`, 'settings.ini')).text;
    await shot(b, 'settings-readonly.png');
    // the owner: settings.ini changed on disk (not through its panel), then Play
    await a.evaluate(async (dir) => {
      const d = await (await navigator.storage.getDirectory()).getDirectoryHandle(dir);
      const f = await d.getFileHandle('settings.ini');
      const t = (await (await f.getFile()).text()).replace(/^#fps = auto$/m, 'fps = 60');
      const w = await f.createWritable(); await w.write(t); await w.close();
    }, `${RUN}-o`);
    await a.click('#play');
    await waitPhase(a, ['playing'], 60000);
    const applied = await a.waitForFunction(() => window.__nfsu2.log.tail(200).some(l => /settings\.ini:\d+: fps -> NFSU2_FPS=60/.test(l)), null, { timeout: 20000 }).then(() => true, () => false);
    await a.evaluate(() => window.__nfsu2.session?.terminate());
    await waitPhase(a, ['exited'], 30000);
    report('readonly', /Read only here/.test(ro.note) && ro.disabled && ro.textareaReadOnly && ro.saveDisabled && before === after && applied,
      { secondTab: ro, fileUnchanged: before === after, ownerPlayReadTheFileAgain: applied });
    await c2.close();
  }

  // ------------------------------------------------------------ retry --
  // the first game.wasm request fails (a Wi-Fi drop): the module check fails on
  // its own, Play stays off, and the page recovers by itself (back-off) or by Retry
  if (want('retry')) {
    const run1 = async (useButton) => {
      const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
      const p = watch(await c2.newPage(), useButton ? 'R2' : 'R1');
      let n = 0;
      await p.route(u => u.pathname === PATH + 'game.wasm', route => (++n === 1 ? route.abort('internetdisconnected') : route.continue()));
      await p.goto(`${BASE}/?save=${RUN}-r${useButton ? 2 : 1}&fresh`);
      await p.waitForFunction(() => window.__nfsu2?.checks().find(c => c.id === 'module')?.state === 'fail', null, { timeout: 30000, polling: 50 });
      const failed = await p.evaluate(() => ({ phase: window.__nfsu2.phase, status: document.querySelector('#status').textContent,
        sub: document.querySelector('#status-sub').textContent, retryShown: !document.querySelector('#retry').hidden, playDisabled: document.querySelector('#play').disabled,
        problem: document.querySelector('#problems li[data-id="module"]')?.textContent ?? null }));
      if (!useButton) await shot(p, 'module-retry.png');
      const t0 = Date.now();
      if (useButton) await p.click('#retry');
      const ph = await waitPhase(p, ['ready'], 30000).catch(() => phase(p));
      const r = { whileFailed: failed, after: ph, recoveredMs: Date.now() - t0, wasmRequests: n,
        retryHiddenAfter: await p.evaluate(() => document.querySelector('#retry').hidden) };
      await c2.close();
      return r;
    };
    // a new build between /game.json and /game.wasm (the server answers 409): the page asks /game.json again at once
    const rebuilt = async () => {
      const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
      const p = watch(await c2.newPage(), 'R3');
      let n = 0, infos = 0;
      await p.route(u => u.pathname === PATH + 'game.json' && !u.search, route => { infos++; return route.continue(); });
      await p.route(u => u.pathname === PATH + 'game.wasm', route => (++n === 1 ? route.fulfill({ status: 409, body: 'The game module changed' }) : route.continue()));
      await p.goto(`${BASE}/?save=${RUN}-r3&fresh`);
      const ph = await waitPhase(p, ['ready'], 30000).catch(() => phase(p));
      const lines = (await p.evaluate(() => window.__nfsu2.log.tail(80))).filter(l => /game module/.test(l));
      await c2.close();
      return { after: ph, wasmRequests: n, gameJsonRequests: infos, lines };
    };
    const auto = await run1(false), button = await run1(true), rb = await rebuilt();
    const ok = (r) => r.whileFailed.status === 'Not ready' && r.whileFailed.retryShown && r.whileFailed.playDisabled && /did not load/.test(r.whileFailed.problem ?? '') &&
      r.after === 'ready' && r.wasmRequests === 2 && r.retryHiddenAfter;
    const rbOk = rb.after === 'ready' && rb.wasmRequests === 2 && rb.gameJsonRequests >= 2 && !rb.lines.some(l => /did not load/.test(l));
    report('retry', ok(auto) && ok(button) && button.recoveredMs < 1500 && rbOk, { byItself: auto, retryButton: button, newBuildInBetween: rb });
  }

  // ----------------------------------------------------------- update --
  if (want('update')) {
    const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const p = watch(await c2.newPage(), 'U');
    // the server's answer to the page's question; with deployed set, it has another version than the page loaded
    let deployed = null, asked = 0;
    await p.route('**/game.json?latest', async route => {
      asked++;
      const r = await route.fetch(), info = await r.json();
      await route.fulfill({ response: r, json: deployed ? { ...info, rev: deployed } : info });
    });
    // (kept: offline mode's copy of game.json, by its address under the page's base: '/game.json' at the root, where
    // sw.ts keeps it)
    const state = () => p.evaluate(async path => ({ newer: window.__nfsu2.newer(), reloadShown: !document.querySelector('#reload').hidden,
      line: document.querySelector('#newer').hidden ? null : document.querySelector('#newer').textContent,
      kept: await (await caches.match(path + 'game.json'))?.json().then(j => j.rev).catch(() => null) ?? null }), PATH);
    const shown = () => p.evaluate(() => document.dispatchEvent(new Event('visibilitychange')));
    await open(p, `save=${RUN}-u&fresh&env=NOMOVIES%3D1`);
    const rev = await p.evaluate(() => window.__nfsu2.checks().find(c => c.id === 'module').label.split(' ').pop());
    await p.click('#play');
    await p.waitForFunction(() => !!window.__nfsu2.session, null, { timeout: 30000, polling: 5 });
    // in a game: nothing while the server has the loaded version; another one: a word over the game, offered at its end
    await shown();
    await p.waitForTimeout(1500);
    const same = { ...await state(), asked };
    deployed = 'upd0000';
    await shown();
    await p.waitForFunction(() => !!window.__nfsu2.newer(), null, { timeout: 10000 }).catch(() => {});
    const inGame = await p.evaluate(() => ({ newer: window.__nfsu2.newer(), toast: document.querySelector('#toast').hidden ? null : document.querySelector('#toast').textContent }));
    await p.evaluate(() => window.__nfsu2.session.terminate());
    await waitPhase(p, ['exited'], 30000);
    const offered = { ...await state(), asked };
    await shot(p, 'update-offer.png');
    // Reload: the server's version is the page's again, and a new one offered before any game has no note on the log
    deployed = null;
    await Promise.all([p.waitForEvent('load'), p.click('#reload')]);
    await waitPhase(p, ['ready'], 60000);
    await shown();
    await p.waitForTimeout(1500);
    const reloaded = await state();
    deployed = 'upd0001';
    await shown();
    await p.waitForFunction(() => !!window.__nfsu2.newer(), null, { timeout: 10000 }).catch(() => {});
    const beforeGame = await state();
    const ok = same.asked >= 2 && !same.newer && !same.reloadShown && same.line === null &&
      inGame.newer === 'upd0000' && /new version/.test(inGame.toast ?? '') &&
      offered.newer === 'upd0000' && offered.reloadShown && /Reload this page/.test(offered.line ?? '') && /Download the log first/.test(offered.line ?? '') &&
      (offered.kept === null || offered.kept === rev) && !reloaded.newer && !reloaded.reloadShown &&
      beforeGame.newer === 'upd0001' && beforeGame.reloadShown && !/Download the log/.test(beforeGame.line ?? '');
    report('update', ok, { rev, same, inGame, offered, reloaded, beforeGame });
    await c2.close();
  }

  // ------------------------------------------------------- autoupdate --
  if (want('autoupdate')) {
    // a player's page (no test settings in the address); no service worker, so the routes see /game.json too
    const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 }, serviceWorkers: 'block' });
    const p = watch(await c2.newPage(), 'AU');
    // deployed: the server's answer to the page's question; served: the version /game.json names to a page loading
    let deployed = null, served = null, asked = 0, loads = 0;
    await p.route('**/game.json?latest', async route => {
      asked++;
      const r = await route.fetch(), info = await r.json();
      await route.fulfill({ response: r, json: deployed ? { ...info, rev: deployed } : info });
    });
    await p.route(u => u.pathname === PATH + 'game.json' && !u.search, async route => {
      const r = await route.fetch(), info = await r.json();
      await route.fulfill({ response: r, json: served ? { ...info, rev: served } : info });
    });
    p.on('load', () => loads++);
    const state = () => p.evaluate(() => ({ rev: window.__nfsu2.checks().find(c => c.id === 'module')?.label.split(' ').pop(),
      newer: window.__nfsu2.newer(), tried: window.__nfsu2.tried(), reloadShown: !document.querySelector('#reload').hidden,
      line: document.querySelector('#newer').hidden ? null : document.querySelector('#newer').textContent,
      key: sessionStorage.getItem('nfsu2-update') }));
    const shown = () => p.evaluate(() => document.dispatchEvent(new Event('visibilitychange')));
    await open(p, '');
    await p.waitForTimeout(1500);                                  // the question once the module has loaded
    const first = { ...await state(), asked };
    // a deploy, and nothing on the page to lose: it reloads by itself into the new version, and says so
    deployed = served = 'upd0002';
    await Promise.all([p.waitForEvent('load', { timeout: 15000 }), shown()]);
    await waitPhase(p, ['ready'], 60000);
    await p.waitForTimeout(1500);
    const auto = { ...await state(), loads };
    // another one while a field has the focus: offered, not reloaded, and Play reloads.  That reload does not bring
    // it (the page still gets upd0002): offered, and the page does not reload by itself again
    await settingsReady(p);
    await p.focus('#set-roam_name');
    deployed = 'upd0003';
    await shown();
    await p.waitForFunction(() => !!window.__nfsu2.newer(), null, { timeout: 10000 }).catch(() => {});
    await p.waitForTimeout(1000);
    const typing = { ...await state(), loads };
    await shot(p, 'autoupdate-offer.png');
    await p.evaluate(() => document.activeElement.blur());
    await Promise.all([p.waitForEvent('load', { timeout: 15000 }), p.click('#play')]);
    await waitPhase(p, ['ready'], 60000);
    await p.waitForFunction(() => !!window.__nfsu2.newer(), null, { timeout: 10000 }).catch(() => {});
    await shown();
    await p.waitForTimeout(3000);
    const notBrought = { ...await state(), loads };
    const ok = first.asked >= 1 && !first.newer && first.line === null &&
      auto.loads === 2 && auto.rev === 'upd0002' && !auto.newer && auto.key === null && !auto.reloadShown && /reloaded for the new version/.test(auto.line ?? '') &&
      typing.loads === 2 && typing.newer === 'upd0003' && typing.reloadShown && /Play reloads/.test(typing.line ?? '') &&
      notBrought.loads === 3 && notBrought.rev === 'upd0002' && notBrought.newer === 'upd0003' && notBrought.tried === 'upd0003' &&
      notBrought.key === 'upd0003' && notBrought.reloadShown && /Reload this page/.test(notBrought.line ?? '');
    report('autoupdate', ok, { first, auto, typing, notBrought });
    await c2.close();
  }

  // -------------------------------------------------------- enterlink --
  if (want('enterlink')) {
    const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const p = watch(await c2.newPage(), 'E');
    await open(p, `save=${RUN}-e&fresh&env=NOMOVIES%3D1`);
    await settingsReady(p);
    await p.focus('#set-roam_name');
    await p.keyboard.press('Enter');
    await p.waitForTimeout(500);
    const onField = { phase: await phase(p), focus: await p.evaluate(() => document.activeElement?.id), stage: await p.evaluate(() => !document.querySelector('#stage').hidden) };
    await p.evaluate(() => document.activeElement?.blur());
    await p.keyboard.press('Enter');
    const onPage = await waitPhase(p, ['playing', 'starting'], 30000).catch(() => phase(p));
    await p.evaluate(() => window.__nfsu2.session?.terminate());
    await waitPhase(p, ['exited'], 30000).catch(() => {});
    report('enterlink', onField.phase === 'ready' && onField.focus === 'set-roam_name' && !onField.stage && (onPage === 'playing' || onPage === 'starting'),
      { enterInField: onField, enterOnPage: onPage });
    await c2.close();
  }

  // ------------------------------------------------------------- late --
  // Stop at once after Play: subsystems still attaching (audio loads its worklet,
  // the renderer waits for its registration) must not outlive the game
  if (want('late')) {
    const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    await c2.addInitScript(RECORD_GPU_AUDIO);
    const p = watch(await c2.newPage(), 'L');
    await open(p, `save=${RUN}-l&fresh&env=NOMOVIES%3D1`);
    await p.click('#play');
    await p.waitForFunction(() => !!window.__nfsu2.session, null, { timeout: 30000, polling: 5 });
    await p.evaluate(() => window.__nfsu2.session.terminate());
    await waitPhase(p, ['exited'], 30000);
    await p.waitForTimeout(4000);
    const tail = (await p.evaluate(() => window.__nfsu2.log.tail(300))).join('\n');
    const end = tail.lastIndexOf('[app] the game exited');
    const after = tail.slice(end);
    const lateAttached = (after.match(/\[(gpu\] renderer attached|input\] attached|audio\] worklet attached)/g) ?? []).length;
    const detachedAtOnce = (after.match(/attached after the game ended: detached at once/g) ?? []).length;
    const ctxClosed = /\[audio\] context closed/.test(tail) || /audio attached after the game ended/.test(after);
    // a renderer still waiting for the game's graphics block when the game ended gives its device back
    const devices = await p.evaluate(() => window.__rv.devices.map(d => ({ destroyed: d.destroyed, lost: d.lost })));
    const devicesOk = devices.every(d => d.destroyed);
    report('late', p && lateAttached <= detachedAtOnce && ctxClosed && devicesOk, { exit: await p.evaluate(() => window.__nfsu2.lastExit), lateAttached, detachedAtOnce, audioContextClosed: ctxClosed, devices,
      linesAfterExit: after.split('\n').filter(l => /\[(app|gpu|audio|input)\]/.test(l)).slice(0, 8) });
    await c2.close();
  }

  // ------------------------------------------------------- quitstages --
  if (want('quitstages')) {
    const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const q = watch(await c2.newPage(), 'Q');
    const quitLabel = async () => {
      await q.keyboard.press('F9');
      await q.waitForSelector('#menu[open]');
      return q.locator('#menu [data-act="quit"]').textContent();
    };
    const tailOf = async () => (await q.evaluate(() => window.__nfsu2.log.tail(600))).join('\n');
    // 1. Quit at once after Play: the game's input mailbox is not there yet (about 0.2 s)
    await open(q, `save=${RUN}-q&fresh&env=NOMOVIES%3D1&env=NFSU2_FNG_LOG%3D1`);
    await q.evaluate(() => window.__nfsu2.log.clear());
    await q.click('#play');
    await q.waitForFunction(() => !!window.__nfsu2.session?.canQuit(), null, { timeout: 30000, polling: 5 });
    const early = await q.evaluate(() => ({ sent: window.__nfsu2.session.requestQuit(), inputUp: window.__nfsu2.session.input?.status().attached }));
    await waitPhase(q, ['exited'], 60000);
    let tail = await tailOf();
    const earlyR = { ...early, exit: await q.evaluate(() => window.__nfsu2.lastExit),
      queued: /quit requested before the game's input is up/.test(tail), sentLater: /sending the quit request made before/.test(tail),
      reachedGame: /quit requested \(page quit request\)/.test(tail) };
    const earlyOk = early.sent === true && early.inputUp === false && earlyR.exit?.code === 0 && earlyR.queued && earlyR.sentLater && earlyR.reachedGame;
    // 2. Quit, then "Close the game now" while it boots: the game's second-request rule (input_web.c force_exit)
    await q.evaluate(() => window.__nfsu2.log.clear());
    await q.click('#play');
    await q.waitForFunction(() => window.__nfsu2.log.tail(300).some(l => /\[input\] attached/.test(l)), null, { timeout: 60000, polling: 20 });
    const l0 = await quitLabel();
    await q.click('#menu [data-act="quit"]');
    const l1 = await quitLabel();
    await q.click('#menu [data-act="quit"]');
    await q.waitForSelector('#confirm[open]');
    const ask1 = await q.locator('#confirm-title').textContent();
    await q.click('#confirm-ok');
    await waitPhase(q, ['exited'], 60000);
    tail = await tailOf();
    const closeR = { labels: [l0, l1], confirm: ask1, exit: await q.evaluate(() => window.__nfsu2.lastExit),
      forced: /forcing the exit \(second quit request\)/.test(tail), headline: await q.locator('#status').textContent() };
    const closeOk = l0 === 'Quit game' && l1 === 'Close the game now' && ask1 === 'Close the game now?' && closeR.exit?.code === 0 && closeR.forced;
    // 3. A game that does not answer either request (its quit requests are swallowed here):
    //    "Stop the game" only after the forced close's longest run, then it is stopped
    await q.evaluate(() => window.__nfsu2.log.clear());
    await q.click('#play');
    await q.waitForFunction(() => window.__nfsu2.log.tail(300).some(l => /\[input\] attached/.test(l)), null, { timeout: 60000, polling: 20 });
    const times = await q.evaluate(() => { const i = window.__nfsu2.session.input; i.requestQuit = () => true; return window.__nfsu2.quitTimes; });
    const s0 = await quitLabel();
    await q.click('#menu [data-act="quit"]');
    await quitLabel();
    await q.click('#menu [data-act="quit"]');
    await q.waitForSelector('#confirm[open]');
    await q.click('#confirm-ok');
    const tForced = Date.now();
    await q.waitForTimeout(Math.max(0, times.forcedCloseMaxMs + 500 - (Date.now() - tForced)));
    const sMid = await quitLabel();                       // after the forced close's longest run: still not "Stop"
    await q.keyboard.press('Escape');
    await q.waitForTimeout(Math.max(0, times.stopAfterMs + 500 - (Date.now() - tForced)));
    const sEnd = await quitLabel();
    await q.click('#menu [data-act="quit"]');
    await q.waitForSelector('#confirm[open]');
    const ask2 = await q.locator('#confirm-title').textContent();
    await q.click('#confirm-ok');
    await waitPhase(q, ['exited'], 30000);
    const stopR = { quitTimes: times, labels: [s0, sMid, sEnd], confirm: ask2, exit: await q.evaluate(() => window.__nfsu2.lastExit),
      headline: await q.locator('#status').textContent() };
    const stopOk = times.stopAfterMs > times.forcedCloseMaxMs && times.forcedCloseMaxMs >= 7000 && s0 === 'Quit game' &&
      sMid === 'Close the game now' && sEnd === 'Stop the game' && ask2 === 'Stop the game now?' && stopR.exit?.code === -1 && stopR.headline === 'Stopped';
    report('quitstages', earlyOk && closeOk && stopOk, { quitAtOnce: earlyR, closeNow: closeR, stop: stopR });
    await c2.close();
  }

  // ---------------------------------------------------------- slowgpu --
  // the WebGPU device is ready only after the game stopped waiting for a renderer
  // (2 s here, 30 s normally): the game marks its block, the late renderer is refused,
  // and the page stops the game and says why
  if (want('slowgpu')) {
    const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    await c2.addInitScript(() => {
      const rd = GPUAdapter.prototype.requestDevice;
      GPUAdapter.prototype.requestDevice = async function (...a) { const d = await rd.apply(this, a); await new Promise(r => setTimeout(r, 10000)); return d; };
    });
    const p = watch(await c2.newPage(), 'G');
    await open(p, `save=${RUN}-g&fresh&env=NOMOVIES%3D1&env=NFSU2_RENDERER_WAIT_MS%3D2000`);
    await p.click('#play');
    const ph = await waitPhase(p, ['exited'], 60000).catch(() => phase(p));
    const tail = (await p.evaluate(() => window.__nfsu2.log.tail(400))).join('\n');
    const r = { phase: ph, exit: await p.evaluate(() => window.__nfsu2.lastExit), headline: await p.locator('#status').textContent(),
      sub: await p.locator('#status-sub').textContent(),
      gameGaveUp: /no renderer attached after \d+ ms/.test(tail), refused: /renderer could not attach: Error: (the game stopped waiting for a renderer|this game process takes no renderer any more)/.test(tail),
      attachedLines: (tail.match(/\[gpu\] renderer attached:/g) ?? []).length, bugReport: await p.locator('#report').isVisible() };
    report('slowgpu', r.phase === 'exited' && r.gameGaveUp && r.refused && r.attachedLines === 0 && r.headline === 'Did not start' &&
      /the graphics could not start\. Restart Chrome, then press Play/.test(r.sub) && r.bugReport, r);
    await c2.close();
  }

  // ---------------------------------------------------------- gpulost --
  // Safari's graphics process went away (a player's iPhone log, 2026-10-24): every frame's createCommandEncoder threw
  // InvalidStateError from then on, device.lost never came, and the picture stayed frozen while the game played on
  if (want('gpulost')) {
    const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    await c2.addInitScript(() => {
      const ce = GPUDevice.prototype.createCommandEncoder;
      GPUDevice.prototype.createCommandEncoder = function (...a) {
        if (window.__gpuGone) throw new DOMException('GPUDevice.createCommandEncoder: Unable to make command encoder.', 'InvalidStateError');
        return ce.apply(this, a);
      };
      // no adapter until the time in sessionStorage (set before the reload): Chrome after a hung Direct3D 12 device
      const ra = GPU.prototype.requestAdapter;
      GPU.prototype.requestAdapter = function (...a) {
        return Date.now() < Number(sessionStorage.getItem('noAdapterUntil') ?? 0) ? Promise.resolve(null) : ra.apply(this, a);
      };
    });
    const p = watch(await c2.newPage(), 'L');
    await open(p, `save=${RUN}-l&fresh&env=NOMOVIES%3D1`);
    await p.click('#play');
    await waitPhase(p, ['playing'], 60000);
    await p.waitForFunction(() => (window.__nfsu2.log.tail(400) ?? []).some(l => /\[gpu\] display \d+ Hz/.test(l)), null, { timeout: 60000, polling: 500 });
    await p.waitForTimeout(3000);
    await p.evaluate(() => { window.__gpuGone = true; });
    const ph = await waitPhase(p, ['exited'], 30000).catch(() => phase(p));
    const tail = (await p.evaluate(() => window.__nfsu2.log.tail(600))).join('\n');
    const r = { phase: ph, headline: await p.locator('#status').textContent(), sub: await p.locator('#status-sub').textContent(),
      failed: (tail.match(/\[gpu\] encode failed: InvalidStateError: GPUDevice\.createCommandEncoder/g) ?? []).length,
      gone: /\[gpu\] device gone: 60 times in a row/.test(tail), stopped: /\[app\] the graphics stopped working: stopping the game/.test(tail),
      bugReport: await p.locator('#report').isVisible() };
    await shot(p, 'gpulost-exited.png');
    // Play reloads the page (a fresh one: the log starts over), and the next Play plays
    const nav = p.waitForEvent('framenavigated', { timeout: 30000 }).then(() => true, () => false);
    await p.evaluate(() => sessionStorage.setItem('noAdapterUntil', String(Date.now() + 15000)));
    await p.click('#play');
    r.reloaded = await nav;
    if (r.reloaded) {
      // no adapter yet: the page says why (not GPU-ADAPTER-WIN's old graphics), and Play comes on once it is back
      r.afterLost = await p.waitForFunction(() => /GPU-ADAPTER-AFTER-LOST/.test(document.querySelector('#problems')?.textContent ?? ''),
        null, { timeout: 15000, polling: 200 }).then(() => true, () => false);
      await shot(p, 'gpulost-no-adapter.png');
      const backRe = /^\[app\] the graphics are back after \d+ s/;
      r.back = await p.waitForFunction(re => window.__nfsu2?.log.tail(50).find(l => new RegExp(re).test(l)) ?? null, backRe.source,
        { timeout: 40000, polling: 500 }).then(h => h.jsonValue(), () => null);
      await waitPhase(p, ['ready', 'error'], 60000);
      const after = await p.evaluate(() => window.__nfsu2.log.tail(50));
      r.logAfter = after.some(l => /the last game lost its graphics/.test(l));
      // the line on the last game says what it held at its last copy (lastgame.ts): the textures, at least
      r.held = after.find(l => /^\[app\] the last game .*; at that copy it held textures [1-9]\d* MB/.test(l)) ?? null;
      await p.click('#play');
      r.playsAgain = await waitPhase(p, ['playing'], 60000).then(() => true, () => false);
    }
    report('gpulost', r.phase === 'exited' && r.headline === 'Stopped' && r.failed >= 1 && r.gone && r.stopped &&
      /the graphics stopped working\. Play reloads this page/.test(r.sub) && r.bugReport && r.reloaded && !r.logAfter && !!r.held && r.afterLost && !!r.back && r.playsAgain, r);
    await c2.close();
  }

  // ------------------------------------------------------------ crash --
  // a trap ends the game with exit 134 (game-worker.ts): the start page says so and
  // points to #bug-reports on Discord; a normal end afterwards takes that line away.
  // The trap and the exits are the game worker's own messages, given to the page
  // from its side (the real game keeps running until the page stops it).
  if (want('crash')) {
    const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    await c2.addInitScript(() => {
      const W = window.Worker, ws = window.__workers = [];
      window.Worker = class extends W { constructor(u, o) { super(u, o); ws.push({ w: this, name: o?.name }); } };
    });
    const x = watch(await c2.newPage(), 'X');
    await open(x, `save=${RUN}-x&fresh&env=NOMOVIES%3D1`);
    const end = async (msgs) => {
      await x.click('#play');
      await waitPhase(x, ['playing'], 60000);
      await x.evaluate(ms => {
        const { w } = window.__workers.findLast(e => e.name === 'game-0');
        for (const data of ms) w.dispatchEvent(new MessageEvent('message', { data }));
      }, msgs);
      await waitPhase(x, ['exited'], 30000);
      return x.evaluate(() => ({ exit: window.__nfsu2.lastExit, headline: document.querySelector('#status').textContent,
        sub: document.querySelector('#status-sub').textContent, bugReport: !document.querySelector('#report').hidden,
        link: document.querySelector('#report a').href }));
    };
    const crashed = await end([{ t: 'trap', tid: 1, msg: 'RuntimeError: memory access out of bounds (app-test)', stack: '' }, { t: 'exit', code: 134 }]);
    const trapLogged = (await x.evaluate(() => window.__nfsu2.log.tail(400))).some(l => /TRAP in thread 1: RuntimeError: memory access out of bounds \(app-test\)/.test(l));
    await shot(x, 'crash-exited.png');
    const closed = await end([{ t: 'exit', code: 0 }]);
    report('crash', crashed.exit?.code === 134 && crashed.headline === 'Stopped' && /error code 134/.test(crashed.sub) && crashed.bugReport &&
      /^https:\/\/discord\.(gg|com)\//.test(crashed.link) && trapLogged && closed.exit?.code === 0 && !closed.bugReport,
      { crashed, trapLogged, closed: { exit: closed.exit, headline: closed.headline, bugReport: closed.bugReport } });
    await c2.close();
  }

  // ------------------------------------------------------------- leak --
  // Three Play -> Stop rounds in one tab with sound on: each ended game's memory
  // (1 GB or more) must be collectable.  Round 2 has free roam on (settings.ini,
  // which Play reads again), so the network link ends too.  Its own Chrome, for --expose-gc.
  if (want('leak')) {
    const b2 = await chromium.launch({ channel: 'chrome', headless: !process.argv.includes('--headed'),
      args: ['--enable-unsafe-webgpu', '--js-flags=--expose-gc'] });
    try {
      const c2 = track(await b2.newContext({ viewport: { width: 1280, height: 800 } }));
      await c2.addInitScript(RECORD_GPU_AUDIO);
      const k = watch(await c2.newPage(), 'K');
      const ROUNDS = 3;
      await open(k, `save=${RUN}-k&fresh&env=NOMOVIES%3D1&env=NFSU2_FNG_LOG%3D1`);
      const gcAll = async () => { for (let i = 0; i < 3; i++) { await k.evaluate(() => { for (let j = 0; j < 5; j++) gc(); }); await k.waitForTimeout(1000); } };
      const measure = () => k.evaluate(async () => { try { return (await performance.measureUserAgentSpecificMemory()).bytes; } catch (e) { return String(e); } });
      await gcAll();
      const before = await measure();
      await k.evaluate(() => { window.__wrs = []; });
      const rounds = [];
      let during = null;
      for (let i = 0; i < ROUNDS; i++) {
        await k.waitForFunction(() => ['ready', 'exited'].includes(window.__nfsu2?.phase) && !document.querySelector('#play').disabled, null, { timeout: 60000 });
        const roam = i === 1;
        await k.evaluate(async ([dir, on]) => {
          const d = await (await navigator.storage.getDirectory()).getDirectoryHandle(dir);
          const f = await d.getFileHandle('settings.ini');
          const t = (await (await f.getFile()).text()).replace(/^#?roam = (on|off)$/m, `roam = ${on ? 'on' : 'off'}`);
          const w = await f.createWritable(); await w.write(t); await w.close();
        }, [`${RUN}-k`, roam]);
        await k.evaluate(() => window.__nfsu2.log.clear());
        await k.click('#play');
        await k.waitForFunction(() => !!window.__nfsu2.session, null, { timeout: 30000, polling: 5 });
        await k.waitForFunction(() => window.__nfsu2.log.tail(2000).some(l => /top package UG_LS_Splash/.test(l)), null, { timeout: 120000 });
        if (roam) await k.waitForFunction(() => window.__nfsu2.log.tail(3000).some(l => /\[net\] on the virtual LAN/.test(l)), null, { timeout: 30000 }).catch(() => {});
        await k.waitForFunction(() => window.__nfsu2.session?.audio?.state() === 'running' &&
          window.__nfsu2.log.tail(2000).some(l => /\[audio\] worklet attached/.test(l)), null, { timeout: 30000 }).catch(() => {});
        if (i === ROUNDS - 1) during = await measure();
        const r = await k.evaluate(() => {
          const s = window.__nfsu2.session;
          window.__wrs.push(new WeakRef(s.proc.memory));
          return { lan: window.__nfsu2.log.tail(3000).find(l => /\[net\] on the virtual LAN/.test(l)) ?? null, audio: s.audio?.state() ?? null, worklet: window.__nfsu2.log.tail(2000).some(l => /\[audio\] worklet attached/.test(l)),
            memoryMB: Math.round(s.proc.memory.buffer.byteLength / 1048576) };
        });
        await k.evaluate(() => window.__nfsu2.session.terminate());
        await waitPhase(k, ['exited'], 30000);
        await k.waitForTimeout(1500);
        r.contextClosed = (await k.evaluate(() => window.__nfsu2.log.tail(400))).some(l => /\[audio\] context closed/.test(l));
        rounds.push(r);
      }
      await gcAll();
      const kept = await k.evaluate(() => window.__wrs.map(w => !!w.deref()));
      const after = await measure();
      const objs = await k.evaluate(() => ({ devicesKept: window.__rv.devices.filter(d => d.ref.deref()).length, devicesDestroyed: window.__rv.devices.filter(d => d.destroyed).length,
        devices: window.__rv.devices.length, contextsKept: window.__rv.ctxs.filter(w => w.deref()).length, contexts: window.__rv.ctxs.length }));
      const MB = 1048576;
      const flat = typeof before === 'number' && typeof after === 'number' && after < before + 64 * MB;
      const sees = typeof during === 'number' && during > before + 512 * MB;       // the measurement does count a game's memory
      // the AudioContexts and GPU devices are collected too: a worklet processor that never
      // returns false keeps its context alive, and in the worklet its views keep the game's
      // memory, which neither the WeakRefs nor measureUserAgentSpecificMemory see
      report('leak', rounds.every((r, i) => r.audio === 'running' && r.worklet && r.contextClosed && !!r.lan === (i === 1)) && kept.every(x => !x) && flat && sees &&
        objs.contexts === ROUNDS && objs.contextsKept === 0 && objs.devices === ROUNDS && objs.devicesKept === 0,
        { rounds, memoryKept: kept, uaMemoryMB: { beforeFirstPlay: +(before / MB).toFixed(1), duringRound3: +(during / MB).toFixed(1), afterAll: +(after / MB).toFixed(1) }, ...objs });
      await c2.close();
    } finally { await b2.close(); }
  }

  // ------------------------------------------------------------ cache --
  // How quick a start is, in a persistent browser profile (Playwright's normal
  // contexts are incognito-like: their HTTP cache does not keep the module):
  // page -> Ready on the first and on a repeat visit, and Play -> the title
  // screen (UG_LS_Splash.fng on top) with an empty and with a warm game-data cache.
  if (want('cache')) {
    const dir = path.join(OUT, 'profile');
    fs.rmSync(dir, { recursive: true, force: true });
    const pc = track(await chromium.launchPersistentContext(dir, { channel: 'chrome', headless: !process.argv.includes('--headed'),
      viewport: { width: 1280, height: 800 }, args: ['--enable-unsafe-webgpu'] }));
    const c = watch(pc.pages()[0] ?? await pc.newPage(), 'C');
    const q = `save=${RUN}-c&env=NOMOVIES%3D1&env=NFSU2_FNG_LOG%3D1`;
    const visit = async (extra) => {
      const t0 = Date.now(); await open(c, q + extra); const ms = Date.now() - t0;
      const line = (await c.evaluate(() => window.__nfsu2.log.tail(40))).find(l => l.includes('game module')) ?? '';
      return { pageToReadyMs: ms, module: line.replace(/^\[app\] game module \S+ /, '') };
    };
    const toTitle = async () => {
      await c.evaluate(() => window.__nfsu2.log.clear());              // only this run's lines count
      const t0 = Date.now();
      await c.click('#play');
      await c.waitForFunction(() => window.__nfsu2.log.tail(400).some(l => /top package UG_LS_Splash\.fng/.test(l)), null, { timeout: 180000, polling: 100 });
      const ms = Date.now() - t0;
      await c.waitForFunction(() => !!window.__nfsu2.session);
      await c.evaluate(() => window.__nfsu2.session.terminate());
      await waitPhase(c, ['exited'], 30000);
      return ms;
    };
    const first = await visit('&fresh');
    const cold = await toTitle();
    const warm = await toTitle();
    const again = await visit('');
    const warm2 = await toTitle();
    const cache = await c.evaluate(async () => { let n = 0; const d = await (await navigator.storage.getDirectory()).getDirectoryHandle('cache');
      for await (const [name, h] of d.entries()) if (name.endsWith('.have')) { const b = new Uint8Array(await (await h.getFile()).arrayBuffer()); n += b.reduce((a, x) => a + (x ? 1 : 0), 0); } return n; });
    await pc.close();
    fs.rmSync(dir, { recursive: true, force: true });
    report('cache', /\b\d+ bytes transferred/.test(again.module) && +(/(\d+) bytes transferred/.exec(again.module)?.[1] ?? 1e9) < 65536 && warm < cold,
      { firstVisit: first, repeatVisit: again, playToTitleMs: { emptyDataCache: cold, warmDataCache: warm, afterReload: warm2 }, cachedMiB: cache });
  }

  // ---------------------------------------------------------- roamask --
  // a player's page: no test settings in the address, so its own save folder in a context of its own
  if (want('roamask')) {
    const c5 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const r = watch(await c5.newPage(), 'R');
    const ini = () => readOpfs(r, 'save', 'settings.ini').then(f => f?.text ?? '');
    const roamLines = t => t.split('\n').filter(l => /^roam(_name)? = /.test(l));
    const asked = () => r.evaluate(() => ({ title: document.querySelector('#confirm-title').textContent,
      field: document.querySelector('#roam-ask-name')?.value ?? null, ok: document.querySelector('#confirm-ok').textContent,
      okClass: document.querySelector('#confirm-ok').className, cancel: document.querySelector('#confirm-cancel').textContent,
      focus: document.activeElement?.id ?? null }));
    const stop = async () => {
      await r.waitForFunction(() => !!window.__nfsu2.session, null, { timeout: 60000 });
      await r.evaluate(() => window.__nfsu2.session.terminate());
      await waitPhase(r, ['exited'], 30000);
    };
    // free roam busy on the server (the hub gives fewer than 24 links a player): not asked, nothing written
    const playersUrl = u => u.pathname === PATH + 'players';
    await r.route(playersUrl, route => route.fulfill({ contentType: 'application/json', body: '{"players":700,"online":40,"full":false,"roamBusy":true}' }));
    await r.goto(`${BASE}/`);
    await waitPhase(r, ['ready', 'error'], 60000);
    await r.waitForFunction(() => /700 players/.test(document.querySelector('#online')?.textContent ?? ''), null, { timeout: 10000 });
    await r.click('#play');
    const busyAsked = await r.waitForSelector('#confirm[open]', { timeout: 4000 }).then(() => true, () => false);
    await waitPhase(r, ['playing', 'exited'], 60000);
    const busy = { asked: busyAsked, ini: roamLines(await ini()), log: (await r.evaluate(() => window.__nfsu2.log.tail(400))).filter(l => /free roam with others/.test(l)) };
    await stop();
    await r.unroute(playersUrl);
    await r.goto(`${BASE}/`);
    await waitPhase(r, ['ready', 'error'], 60000);
    // a new player: no name line; Esc is Not now
    await r.click('#play');
    await r.waitForSelector('#confirm[open]', { timeout: 10000 });
    const first = await asked();
    await r.locator('#confirm').screenshot({ path: path.join(OUT, 'roam-ask.png') });
    await r.keyboard.press('Escape');
    await waitPhase(r, ['playing', 'exited'], 60000);
    const afterEsc = roamLines(await ini());
    await stop();
    // once only
    await r.click('#play');
    const again = await r.waitForSelector('#confirm[open]', { timeout: 4000 }).then(() => true, () => false);
    await waitPhase(r, ['playing', 'exited'], 60000);
    await stop();
    // a player with a name (Your name): the name to change
    const t = (await ini()).replace(/^roam = off$/m, '#roam = off').replace(/^#roam_name =$/m, 'roam_name = Racer');
    await r.evaluate(async t => { const w = await (await (await (await navigator.storage.getDirectory()).getDirectoryHandle('save')).getFileHandle('settings.ini')).createWritable(); await w.write(t); await w.close(); }, t);
    await r.click('#play');
    await r.waitForSelector('#confirm[open]', { timeout: 10000 });
    const named = await asked();
    await r.locator('#confirm').screenshot({ path: path.join(OUT, 'roam-ask-named.png') });
    await r.fill('#roam-ask-name', 'Zoë');
    await r.click('#confirm-ok');
    await r.waitForTimeout(300);
    const refused = await r.evaluate(() => ({ open: document.querySelector('#confirm').open, why: document.querySelector('#roam-ask-name').validationMessage }));
    await r.fill('#roam-ask-name', 'New Racer');
    await r.press('#roam-ask-name', 'Enter');
    // the phase is still the last game's 'exited' until the answer is written and this game starts
    await r.waitForFunction(() => window.__nfsu2.phase !== 'exited', null, { timeout: 10000 });
    await waitPhase(r, ['playing', 'exited'], 60000);
    const afterOn = roamLines(await ini());
    const env = await r.evaluate(() => window.__nfsu2.env());
    await stop();
    await c5.close();
    report('roamask', !busy.asked && busy.ini.length === 0 && busy.log.length === 1 && /not asked, the server's free roam is busy/.test(busy.log[0]) &&
      first.title === 'Free roam with others?' && first.field === null && first.ok === 'Turn on' && first.okClass === 'btn selected' &&
      first.cancel === 'Not now' && first.focus === 'confirm-ok' && afterEsc.join() === 'roam = off' && !again && named.field === 'Racer' &&
      refused.open && /not a name/.test(refused.why) && [...afterOn].sort().join() === 'roam = on,roam_name = New Racer' && env.NFSU2_ROAM === '1' && env.NFSU2_ROAM_NAME === 'New Racer',
      { busy, first, afterEsc, askedAgain: again, named, refused, afterOn, env: { NFSU2_ROAM: env.NFSU2_ROAM, NFSU2_ROAM_NAME: env.NFSU2_ROAM_NAME } });
  }

  // --------------------------------------------------------- cloudask --
  // a player's page (no test settings: the question is not asked on a test page) in a context of its own
  if (want('cloudask')) {
    const c6 = await browser.newContext({ viewport: { width: 1280, height: 800 }, acceptDownloads: true, permissions: ['clipboard-write'] });
    const r = watch(await c6.newPage(), 'C');
    const KEY = 'nfsu2-cloud-offer:save';
    const count = () => r.evaluate(k => localStorage.getItem(k), KEY);
    const setCount = v => r.evaluate(([k, v]) => localStorage.setItem(k, v), [KEY, v]);
    // a profile file is all the question looks for (the game is stopped before it reads one)
    const profile = on => r.evaluate(async on => {
      const save = await (await navigator.storage.getDirectory()).getDirectoryHandle('save', { create: true });
      if (!on) { await save.removeEntry('NFS Underground 2', { recursive: true }).catch(() => {}); return; }
      const d = await (await save.getDirectoryHandle('NFS Underground 2', { create: true })).getDirectoryHandle('CLOUDT', { create: true });
      const w = await (await d.getFileHandle('CLOUDT', { create: true })).createWritable(); await w.write(new Uint8Array(4096).fill(7)); await w.close();
    }, on);
    const title = () => r.evaluate(() => document.querySelector('#confirm[open]') ? document.querySelector('#confirm-title').textContent : null);
    const stop = async () => {
      await r.waitForFunction(() => !!window.__nfsu2.session, null, { timeout: 60000 });
      await r.evaluate(() => window.__nfsu2.session.terminate());
      await waitPhase(r, ['exited'], 30000);
    };
    // Play, every question answered with Esc: the titles asked
    const playAll = async () => {
      const asked = [];
      const p0 = await phase(r);
      await r.click('#play');
      for (;;) {
        const t = await r.waitForFunction(p0 => document.querySelector('#confirm[open]') ? document.querySelector('#confirm-title').textContent
          : window.__nfsu2.phase !== p0 && ['playing', 'exited'].includes(window.__nfsu2.phase) ? 1 : null, p0, { timeout: 60000, polling: 100 }).then(h => h.jsonValue());
        if (t === 1) break;
        asked.push(t);
        await r.keyboard.press('Escape');
        await r.waitForFunction(() => !document.querySelector('#confirm[open]'), null, { timeout: 5000 });
      }
      await stop();
      return { asked, count: await count() };
    };
    await r.goto(`${BASE}/`);
    await waitPhase(r, ['ready', 'error'], 60000);
    await r.waitForFunction(() => !document.querySelector('#vault').hidden, null, { timeout: 10000 });
    await profile(true); await setCount('4');
    const withRoam = await playAll();                      // the first Play asks about free roam: that one only
    await profile(false);
    const noProfile = await playAll();
    await profile(true); await setCount('3');
    const fourth = await playAll();
    // the fifth: asked; Esc is Not now
    await r.click('#play');
    await r.waitForSelector('#confirm[open]', { timeout: 10000 });
    const first = await r.evaluate(() => ({ title: document.querySelector('#confirm-title').textContent, text: document.querySelector('#confirm-body').innerText,
      ok: document.querySelector('#confirm-ok').textContent, cancel: document.querySelector('#confirm-cancel').textContent,
      cancelShown: !document.querySelector('#confirm-cancel').hidden, focus: document.activeElement?.id ?? null }));
    await r.locator('#confirm').screenshot({ path: path.join(OUT, 'cloud-ask.png') });
    await r.keyboard.press('Escape');
    // the phase is still the last game's 'exited' until this game starts
    await r.waitForFunction(() => window.__nfsu2.phase !== 'exited', null, { timeout: 10000 });
    await waitPhase(r, ['playing', 'exited'], 60000);
    const afterEsc = { count: await count(), link: await r.evaluate(() => localStorage.getItem('nfsu2-keyphrase:save')) };
    await stop();
    const again = await playAll();
    // another player's fifth: Turn on
    await setCount('4');
    await r.click('#play');
    await r.waitForSelector('#confirm[open]', { timeout: 10000 });
    await r.click('#confirm-ok');
    await r.waitForFunction(() => document.querySelector('#confirm[open]') && document.querySelector('#confirm-title').textContent === 'Your keyphrase', null, { timeout: 30000 });
    // every edge on the 4 px grid, inside the dialog's border, and nothing wider than it, at both widths
    const grid = () => r.evaluate(() => {
      const dlg = document.querySelector('#confirm'), d = dlg.getBoundingClientRect(), top = d.top + dlg.clientTop, off = [];
      for (const el of document.querySelectorAll('#confirm h2, #confirm-body p, #confirm-body .row, #confirm-body button, #confirm .row.end')) {
        if (!el.offsetParent) continue;
        const b = el.getBoundingClientRect();
        for (const [k, v] of [['top', b.top - top], ['bottom', b.bottom - top]]) if (Math.round(v) % 4) off.push(`${el.tagName.toLowerCase()}.${el.className || el.id} ${k} ${v}`);
        if (b.right > d.right - 24 + 0.5) off.push(`${el.tagName.toLowerCase()}.${el.className} past the dialog's padding`);
      }
      return { off, width: d.width, overflow: document.documentElement.scrollWidth > innerWidth };
    });
    const shown = await r.evaluate(() => ({ text: document.querySelector('#confirm-body').innerText, phrase: document.querySelector('#confirm-body .keyphrase')?.textContent ?? '',
      ok: document.querySelector('#confirm-ok').textContent, cancelShown: !document.querySelector('#confirm-cancel').hidden, focus: document.activeElement?.id ?? null }));
    const wide = await grid();
    await r.locator('#confirm').screenshot({ path: path.join(OUT, 'cloud-keyphrase.png') });
    await r.setViewportSize({ width: 390, height: 844 });
    await r.waitForTimeout(200);
    const narrow = await grid();
    await r.locator('#confirm').screenshot({ path: path.join(OUT, 'cloud-keyphrase-390.png') });
    await r.setViewportSize({ width: 1280, height: 800 });
    await r.click('#confirm-body button:has-text("Copy")');
    const copied = await r.waitForFunction(() => document.querySelector('#confirm-body button')?.textContent === 'Copied.', null, { timeout: 3000 }).then(() => true, () => false);
    const dl = r.waitForEvent('download');
    await r.click('#confirm-body button:has-text("Download keyphrase")');
    const file = fs.readFileSync(await (await dl).path(), 'utf8');
    const stillOpen = await title();
    await r.click('#confirm-ok');
    await r.waitForFunction(() => window.__nfsu2.phase !== 'exited', null, { timeout: 10000 });
    await waitPhase(r, ['playing', 'exited'], 60000);
    const played = await phase(r);
    const after = await r.evaluate(() => ({ link: JSON.parse(localStorage.getItem('nfsu2-keyphrase:save') ?? 'null'), count: localStorage.getItem('nfsu2-cloud-offer:save'),
      when: document.querySelector('#vault-when').textContent }));
    await stop();
    const cloudLog = (await r.evaluate(() => window.__nfsu2.log.tail(2000))).filter(l => /cloud saves:|keyphrase:/.test(l));
    // the cloud copy goes (the dev server's state stays clean)
    await r.click('#vault-delete');
    await r.waitForSelector('#confirm[open]', { timeout: 5000 });
    await r.click('#confirm-ok');
    const deleted = await r.waitForFunction(() => /Deleted from the cloud/.test(document.querySelector('#vault-msg').textContent), null, { timeout: 15000 }).then(() => true, () => false);
    await c6.close();
    const ROAM = 'Free roam with others?', ASK = 'Turn on cloud saves?';
    report('cloudask', withRoam.asked.join() === ROAM && withRoam.count === '5' && noProfile.asked.length === 0 && noProfile.count === '6' &&
      fourth.asked.length === 0 && fourth.count === '4' && first.title === ASK && first.ok === 'Turn on' && first.cancel === 'Not now' && first.cancelShown &&
      first.focus === 'confirm-ok' && afterEsc.count === 'asked' && afterEsc.link === null && again.asked.length === 0 &&
      /^nfsu2-xyz(-[a-z]+){8}$/.test(shown.phrase) && shown.ok === 'Continue' && !shown.cancelShown && shown.focus === 'confirm-ok' &&
      !wide.off.length && !narrow.off.length && !narrow.overflow && copied && file.trim() === shown.phrase && stillOpen === 'Your keyphrase' &&
      played === 'playing' && after.link?.phrase === shown.phrase && after.count === 'asked' && /^Synced/.test(after.when) &&
      cloudLog.some(l => /cloud saves: not now/.test(l)) && cloudLog.some(l => /cloud saves: turned on/.test(l)) && deleted,
      { withRoam, noProfile, fourth, first, afterEsc, again, shown, wide, narrow, copied, file: file.trim(), stillOpen, played,
        after: { phrase: after.link?.phrase, count: after.count, when: after.when }, cloudLog, deleted });
  }

  // ------------------------------------------------------------- full --
  // a player's page on a full server (--max-peers 1, the one place held here by a tool's connection to /lan)
  if (want('full')) {
    const hold = () => new Promise((res, rej) => {
      const ws = new WebSocket(`${BASE.replace(/^http/, 'ws')}/lan`);
      ws.on('message', d => { if (JSON.parse(String(d)).t === 'welcome') res(ws); });
      ws.on('close', (code, why) => rej(new Error(`the place was not ours: ${code} ${why}`)));
    });
    const c6 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const f = watch(await c6.newPage(), 'F');
    const dialog = () => f.evaluate(() => ({ title: document.querySelector('#confirm-title').textContent,
      text: document.querySelector('#confirm-body').textContent, ok: document.querySelector('#confirm-ok').textContent,
      cancel: document.querySelector('#confirm-cancel').textContent }));
    const line = () => f.evaluate(() => { const q = document.querySelector('#queue'); return q.hidden ? null : q.textContent; });
    const stop = async () => {
      await f.waitForFunction(() => !!window.__nfsu2.session, null, { timeout: 60000 });
      await f.evaluate(() => window.__nfsu2.session.terminate());
      await waitPhase(f, ['exited'], 30000);
    };
    let holder = await hold();
    await f.goto(`${BASE}/`);
    await waitPhase(f, ['ready', 'error'], 60000);
    await f.waitForFunction(() => /server is full/.test(document.querySelector('#online')?.textContent ?? ''), null, { timeout: 20000 });
    // the first Play: the free-roam question, saying the server is full; turned on, nothing more is asked
    await f.click('#play');
    await f.waitForSelector('#confirm[open]', { timeout: 10000 });
    const roamAsk = await dialog();
    await f.locator('#confirm').screenshot({ path: path.join(OUT, 'full-roam-ask.png') });
    await f.click('#confirm-ok');
    const second = await f.waitForFunction(() => document.querySelector('#confirm').open, null, { timeout: 1500 }).then(() => true, () => false);
    await waitPhase(f, ['playing', 'exited'], 60000);
    // in line: the place over the game, and the menu's Stop waiting
    await f.waitForFunction(() => !document.querySelector('#queue').hidden, null, { timeout: 60000 });
    const inLine = await line();
    await shot(f, 'full-in-line.png');
    await f.keyboard.press('F9');
    await f.waitForSelector('#menu[open]');
    const stopShown = await f.evaluate(() => !document.querySelector('#menu [data-act="offline"]').hidden);
    await f.locator('#menu').screenshot({ path: path.join(OUT, 'full-menu.png') });
    await f.click('#menu [data-act="resume"]');
    // the place frees up (its holder lets go as a page does at the game's end: a dropped one keeps it a minute): in, the
    // line gone
    holder.close(1000, 'closed');
    await f.waitForFunction(() => document.querySelector('#queue').hidden && !!window.__nfsu2.session?.net?.vip(), null, { timeout: 20000 });
    const inLog = (await f.evaluate(() => window.__nfsu2.log.tail(300))).filter(l => /in line|in from the line|freed up/.test(l));
    await stop();
    // a later Play on a full server: wait in line or play offline
    holder = await hold();
    await f.click('#play');
    await f.waitForSelector('#confirm[open]', { timeout: 10000 });
    const fullAsk = await dialog();
    await f.locator('#confirm').screenshot({ path: path.join(OUT, 'full-ask.png') });
    await f.click('#confirm-cancel');
    await waitPhase(f, ['playing', 'exited'], 60000);
    await f.waitForTimeout(3000);
    const offline = await f.evaluate(() => ({ net: !!window.__nfsu2.session?.net, line: !document.querySelector('#queue').hidden }));
    await stop();
    // Esc waits in line; Stop waiting leaves it for the rest of the game
    await f.click('#play');
    await f.waitForSelector('#confirm[open]', { timeout: 10000 });
    await f.keyboard.press('Escape');
    await waitPhase(f, ['playing', 'exited'], 60000);
    await f.waitForFunction(() => !document.querySelector('#queue').hidden, null, { timeout: 60000 });
    await f.keyboard.press('F9');
    await f.waitForSelector('#menu[open]');
    await f.click('#menu [data-act="offline"]');
    await f.waitForTimeout(1500);
    const left = await f.evaluate(() => ({ line: !document.querySelector('#queue').hidden, toast: document.querySelector('#toast').textContent,
      log: window.__nfsu2.log.tail(100).filter(l => /stopped waiting/.test(l)).length }));
    await stop();
    holder.close(1000, 'closed');
    await c6.close();
    report('full', /The server is full right now/.test(roamAsk.text) && roamAsk.ok === 'Turn on' && !second &&
      /^The server is full\. In line for a place: next/.test(inLine ?? '') && stopShown && inLog.some(l => /in from the line/.test(l)) &&
      fullAsk.title === 'The server is full' && fullAsk.ok === 'Wait in line' && fullAsk.cancel === 'Play offline' &&
      !offline.net && !offline.line && !left.line && /Playing offline/.test(left.toast) && left.log === 1,
      { roamAsk, second, inLine, stopShown, inLog, fullAsk, offline, left });
  }

  // ------------------------------------------------------------- page --
  // nothing about the server for anyone, no certificate pages; every setting is named and described
  // (a label or its group's name, its help line); the controls sheet on the page and in F9's dialog
  if (want('page')) {
    const c2 = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const s = watch(await c2.newPage(), 'P');
    const asked = [];
    s.on('request', r => asked.push(new URL(r.url()).pathname));
    await open(s, `save=${RUN}-p&fresh&env=NOMOVIES%3D1`);
    await settingsReady(s);
    const pageAsked = [...asked];                            // what the page asked for, before this test's own probes
    const gone = await s.evaluate(async p => Object.fromEntries(await Promise.all(['server-info', 'setup', 'ca.crt']
      .map(async u => [p + u, (await fetch(p + u, { cache: 'no-store' })).status]))), PATH);
    const a11y = await s.evaluate(() => {
      const unnamed = [], undescribed = [];
      for (const el of document.querySelectorAll(':is(#settings-body, #online-body) .set :is(input, [role="radiogroup"])')) {
        if (el.type === 'radio') continue;                           // named by its own label, the group by the row's
        const named = el.labels?.length || el.getAttribute('aria-labelledby') && document.getElementById(el.getAttribute('aria-labelledby'))?.textContent;
        if (!named) unnamed.push(el.id || el.outerHTML.slice(0, 60));
        const desc = (el.getAttribute('aria-describedby') ?? '').split(' ').filter(Boolean);
        if (!desc.length || desc.some(id => !document.getElementById(id))) undescribed.push(el.id || el.outerHTML.slice(0, 60));
      }
      return { unnamed, undescribed, keys: document.querySelectorAll('#keys .keys > div').length, dialogsOpen: document.querySelectorAll('dialog[open]').length,
        headings: [...document.querySelectorAll('h1, h2')].map(h => `${h.tagName}:${h.textContent}`) };
    });
    await s.click('#play');
    await waitPhase(s, ['playing'], 60000);
    await s.keyboard.press('F9');
    await s.waitForSelector('#menu[open]');
    await s.click('#menu [data-act="help"]');
    await s.waitForSelector('#help[open]');
    const helpKeys = await s.locator('#help-body .keys > div').count();
    const inert = await s.evaluate(() => document.querySelector('#app').inert);
    await shot(s, 'help-dialog.png');
    // the dialog's copy of the sheet has tabs of its own (controls.ts): the controller's shows its drawing, and no
    // id of the copy is the page's too
    await s.click('#help-body [role="tab"][data-tab="pad"]');
    const helpTabs = await s.evaluate(() => {
      const ids = [...document.querySelectorAll('[id]')].map(e => e.id);
      return { padShown: document.querySelector('#help-body .pad')?.checkVisibility() ?? false,
        pageTab: document.querySelector('#keys [aria-selected="true"]')?.dataset.tab, dupIds: ids.filter((id, i) => ids.indexOf(id) !== i) };
    });
    await shot(s, 'help-dialog-pad.png');
    await s.keyboard.press('Escape');
    await s.evaluate(() => window.__nfsu2.session?.terminate());
    await waitPhase(s, ['exited'], 30000);
    const inertAfter = await s.evaluate(() => document.querySelector('#app').inert);
    report('page', Object.values(gone).every(c => c === 404) && !pageAsked.some(u => ['server-info', 'setup', 'ca.crt'].some(n => u === PATH + n)) &&
      !a11y.unnamed.length && !a11y.undescribed.length && a11y.keys >= 10 && helpKeys === a11y.keys && !a11y.dialogsOpen && inert && !inertAfter &&
      helpTabs.padShown && helpTabs.pageTab === 'keys' && !helpTabs.dupIds.length,
      { gone, ...a11y, helpKeys, helpTabs, startPageInertWhilePlaying: inert, inertAfter });
    await c2.close();
  }

  // ----------------------------------------------------------- mobile --
  if (want('mobile')) {
    const c3 = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: false });
    const m = watch(await c3.newPage(), 'M');
    await open(m, `save=${RUN}-m&fresh`);
    await m.waitForTimeout(1200);
    const overflow = await m.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
    await shot(m, 'mobile.png');
    report('mobile', overflow <= 0, { horizontalOverflowPx: overflow });
    await c3.close();
  }
} catch (e) {
  report('error', false, String(e?.stack ?? e));
} finally {
  if (REQUESTS) {
    // the page never asks for anything outside its base: no other origin, no path outside it (data: and blob:
    // addresses stay in the browser)
    fs.writeFileSync(REQUESTS, requests.join('\n') + '\n');
    const outside = requests.filter(l => { const a = l.slice(l.indexOf(' ') + 1); return !a.startsWith(PATH) && !/^(data|blob):/.test(a); });
    report('requests', requests.length > 0 && !outside.length, { requests: requests.length, outside: outside.slice(0, 20), file: REQUESTS });
  }
  fs.writeFileSync(path.join(OUT, 'console.log'), logs.join('\n'));
  fs.writeFileSync(path.join(OUT, 'results.json'), JSON.stringify(results, null, 1));
  // the test's OPFS folders go with the browser profile (Playwright's is temporary)
  await browser.close();
  const failed = results.filter(r => !r.ok).length;
  console.log(`${results.length - failed}/${results.length} passed; screenshots and logs in ${OUT}`);
  process.exit(failed ? 1 : 0);
}
