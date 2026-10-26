// The game's Online mode between browsers: one to six game instances in
// headless Chrome pages (separate browser contexts, so separate OPFS) on one
// local server, logging into the server's own online host (web/server-rs/crates/online)
// and meeting in its lobby: the online version of web/tools/lan-pair.mjs.
// Instance 1 creates a game, the others find it, join and ready; the game's AI
// drives every car (drag: the synthetic pad).  Scripts come from
// onlineScript() below, or from files (--script, one per instance in order).
//
//   node web/tools/online-pair.mjs --out <dir> [--players 1..6] [--mode circuit|sprint|drag|drift|streetx] [--races N]
//        [--http-port 8673] [--base http://localhost:8671] [--timeout <s>] [--stagger <s>]
//        [--script tests/scripts/x.txt ...]   (instead of the generated flow)
//        [--env K=V ...] [--env-i<n> K=V ...] [--headed] [--server-arg X ...] (own server only)
//        [--ranked]  the host creates a ranked game (Options: Game Type)
//        [--horn]    in the race each instance holds N (its horn, runtime/racehorn.c) for a second, one after the
//                    other: instance n 3 + 4 (n - 1) s after the start; the others' logs say "[horn] <name>'s horn"
//        [--quit-mid N:MS]   instance N's game quits MS after its race started (the others race on)
//        [--wait-room MS] [--join-wait MS]   host: time in the room before Start Race (default 70000); joiners:
//                    time in the lobby before Refresh and Join Game (default 40000)
//        [--ea-login]  through the game's EA Login screens (no player name: the instances' fresh saves have no
//                    profile, so the game shows them): account name, password, persona; without it each instance
//                    plays under its name (NFSU2_ROAM_NAME) and Online goes straight to the Online Main Menu
//        [--password <text>]   what every instance types as its password with --ea-login (default: a fixed test word)
//        [--canary]  privacy check (implies --ea-login): the password (use a unique one, e.g. CANARY-PASSWORD-123) must appear in no
//                    server output, trace, page log or file under the server's state directory, the trace
//                    must show PASS only as <redacted>, and the server's [online]/[lan] lines must name no
//                    address but virtual ones (10.77.0.0/21)
//        [--page play [--size 960x540] [--play-only N]]   the renderer's test page (frame dumps to
//                    <out>/i<n>-*.png through the server's /artifacts/); else the null platform
//        [--page app]  each instance is the player's start page (web/index.html): Play is clicked; the
//                    renderer, audio, input and network are the app's (the name from roam_name, free roam off)
//        [--hide N[:FROM[:TO]]]   instance N's tab goes to the background (another tab in front: really hidden,
//                    no animation frames and throttled timers, as in Chrome for a player) when its script logs
//                    FROM (default: the host race-start, a joiner ready) and comes back when it logs TO (default
//                    results).  This tool starts Chrome itself then (Playwright keeps its own pages visible) and
//                    that instance runs without NOFOCUS, so the game hears the page go to the background.
//                    Fails unless the game was told (app: deactivated) and still made frames there (its [status]
//                    lines, the fps in summary.json), as the game does in a network race.
//        [--game ug2|mw]   the game (ug2 unless given): its generated flow, modes and screens; Most Wanted has
//                    none yet (and the server no Online mode for it)
//        [--help]    this text and the game's modes
//
// Without --base it starts its own server (web/tools/server-cmd.mjs, --dev, the
// game folder from --data or NFSU2_DATA; --online-trace <out>/trace.txt) and
// stops it at the end.
// Writes <out>/i<n>/log.txt (the game's output), <out>/i<n>/page.log (the
// page console with time stamps and NETSTAT lines), <out>/i<n>.txt (the
// scripts), server.log, trace.txt and summary.json.  PASS (the generated race
// flow): every instance exits 0 through its script, reaches the online main
// menu (UI_OLMain/UI_OLMAIN), the game room and the results screen, and logs no
// fault / timeout / trap / unresolved call / "[net] BLOCKED" / page error.
import { chromium } from 'playwright';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { serverCommand } from './server-cmd.mjs';
import { GAMES, gameOf, usage, nothingFor, requireGame } from './rig-game.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d; };
const opts = (k) => argv.flatMap((a, i) => (a === '--' + k && i + 1 < argv.length ? [argv[i + 1]] : []));
// each game's Online races: the generated flow (its screens, onlineScript() below), the modes in Create Game's
// order and the screens the judgement looks for
const ONLINE_OF = {
  ug2: {
    flow: onlineScript, modes: { circuit: 0, sprint: 1, drag: 2, drift: 3, streetx: 4 },
    main: /screen UI_OLMA[Ii][Nn]\.fng/, room: 'screen PC_OL_GameRoom.fng',
    results: /screen UI_PostRaceResults\.fng|screen UI_PostRace_TournResults\.fng/,
  },
};
const NONE = 'none yet: the server has no Online mode for Most Wanted (M9-07), nor this tool its screens (M9-17)';
const GAME = gameOf(argv), G = GAMES[GAME], OL = ONLINE_OF[GAME];
if (argv.includes('--help')) usage(import.meta.url, GAME, 'Modes (--mode)', Object.keys(OL?.modes ?? {}), NONE);
if (!OL) nothingFor(GAME, NONE);
const HTTP_PORT = +opt('http-port', 8673);
const OWN = !argv.includes('--base');
const BASE = opt('base', `http://localhost:${HTTP_PORT}`);
if (!OWN) await requireGame(BASE, GAME);
const DATA = opt('data', process.env.NFSU2_DATA || path.join(os.homedir(), 'projects/nsfu2/Need for Speed - Underground 2'));
const OUT = path.resolve(opt('out', path.join(REPO, 'out/web/online-pair')));
const FILES = opts('script');
const PLAYERS = Math.min(6, Math.max(1, +opt('players', FILES.length || 2)));
const MODE = opt('mode', 'circuit'), RACES = +opt('races', 1);
const TMO = +opt('timeout', 900 * RACES) * 1000;
const STAGGER = +opt('stagger', 3) * 1000;
const PAGE = opt('page', 'hostcheck'), SIZE = opt('size', '960x540').split('x').map(Number), PLAY_ONLY = +opt('play-only', 0);
const PASSWORD = opt('password', 'testpass');
const WAIT_ROOM = +opt('wait-room', 70000), JOIN_WAIT = +opt('join-wait', 40000);
const MODES = OL.modes;
const RANKED = argv.includes('--ranked');
const HORN = argv.includes('--horn');
const EA_LOGIN = argv.includes('--ea-login') || argv.includes('--canary');
// --quit-mid N:MS: instance N quits its game MS after its race started (the others race on)
const QUIT_MID = opt('quit-mid', '') ? opt('quit-mid', '').split(':').map(Number) : [0, 0];
// --hide N[:FROM[:TO]]: instance N's tab in the background from its script's "log FROM" to "log TO"
const HIDE = opt('hide', '') ? (([n, from, to]) => ({ n: +n, from: from || (+n === 1 ? 'race-start' : 'ready'), to: to || 'results' }))(opt('hide', '').split(':')) : null;
fs.mkdirSync(OUT, { recursive: true });

const NAMES = ['OLHOST', 'OLJOIN', 'OLJOIN3', 'OLJOIN4', 'OLJOIN5', 'OLJOIN6'];
const scripts = [];
for (let i = 0; i < PLAYERS; i++) {
  const f = FILES[i] ?? FILES[FILES.length - 1];
  scripts.push(f ? fs.readFileSync(path.resolve(REPO, f), 'utf8') : OL.flow(i === 0 ? 'host' : 'join', NAMES[i], i + 1));
  fs.writeFileSync(path.join(OUT, `i${i + 1}.txt`), scripts[i]);
}

/**
 * The generated flow (the game's screens):
 * main menu -> Online -> the Online Main Menu, logged in under the instance's
 * name (NFSU2_ROAM_NAME, runtime/online.c).  With --ea-login: main menu ->
 * Online -> EA Login: Use Existing EA Account -> account name and password
 * (${NFSU2_OL_NAME}, ${NFSU2_OL_PASS}: the page's environment, so the
 * password is in no script file) -> Continue -> "EA Login cannot be saved
 * without a profile" No -> the persona -> Online Main Menu.  Then Lobby (Game
 * Session Lobby).  host: Create Game -> mode, the first track, options
 * -> "Entering a game room..." OK -> Created Game Room -> waits for the
 * joiners -> Start Race.  join: waits, Refresh, Join Game (the first game) ->
 * the room -> a chat line -> Ready.  Then as lan-pair: the game's AI drives
 * (drag: the synthetic pad) to the results screen.
 */
function onlineScript(role, name, inst) {
  const host = role === 'host', L = [], w = l => L.push(l);
  const raceMs = { circuit: 150000, sprint: 90000, drag: 40000 }[MODE] ?? 150000;
  w(`# Online race, ${role} side (${name}), generated by web/tools/online-pair.mjs --mode ${MODE}`);
  w('0     tapuntil RETURN MC_Background.fng 120 4000');
  w('+200  tap RETURN');
  w('+0    waitfng GarageMain.fng 3000');
  w('+300  tap RIGHT');                      // Quick Race -> Online
  w('+120  tap RETURN');
  if (!EA_LOGIN) w('+0    waitfng UI_OLMAIN.fng 1800');
  else eaLogin(w);
  w('+300  log online-main');
  w('+0    dump online_main');
  w('+0    tap RIGHT');                      // News -> Lobby
  w('+60   tap RETURN');
  w('+0    waitfng PC_OL_Lobby.fng 900');
  w('+600  log lobby');
  for (let race = 1; race <= RACES; race++) if (!raceFlow(w, host, race, raceMs, inst)) return L.join('\n') + '\n';
  if (host) {
    // the results in the game's own screens: Personal Stats, then the overall Circuit board
    w('+0    log rankings');
    w('+0    tap ESCAPE');                   // the lobby's Back: the Online Main Menu (Lobby selected)
    w('+0    waitfng UI_OLMAIN.fng 900');
    w('+300  tap RIGHT');
    w('+40   tap RIGHT');
    w('+60   tap RETURN');
    w('+0    waitfng UI_OLRankings.fng 900');
    w('+300  tap RETURN');                   // Personal Stats
    w('+600  dump personal_stats');
    w('+0    tap ESCAPE');
    w('+300  tap RIGHT');                    // Overall Rankings
    w('+60   tap RETURN');
    w('+900  dump overall');
    w('+0    tap RIGHT');
    w('+300  dump overall_2');
  }
  w('+60   quit');
  return L.join('\n') + '\n';
}
/** the game's EA Login screens, up to the Online Main Menu */
function eaLogin(w) {
  w('+0    waitfng UI_OLEALogin.fng 1500');
  w('+200  tap DOWN');                       // Use Existing EA Account
  w('+60   tap RETURN');
  w('+0    waitfng UI_OLUseExisting.fng 1500');
  w('+300  tap RETURN');                     // EA Account Name: edit, type, done
  w('+120  type ${NFSU2_OL_NAME}');
  w('+60   tap RETURN');
  w('+120  tap DOWN');                       // Password
  w('+60   tap RETURN');
  w('+120  type ${NFSU2_OL_PASS}');
  w('+60   tap RETURN');
  w('+120  tap DOWN');                       // Continue
  w('+40   tap DOWN');
  w('+40   tap DOWN');
  w('+60   dump account');
  w('+0    tap RETURN');
  w('+0    waitfng UI_OLSelectPersona.fng 1800');
  w('+300  dump no_profile');
  w('+0    tap RETURN');                     // "cannot be saved without a profile": No
  w('+200  dump persona');
  w('+0    tap RETURN');                     // the first persona
  w('+0    waitfng UI_OLMAIN.fng 1800');
}
/** one race of the session: create (host) or find and join (joiners), ready, race, results, back to the lobby */
function raceFlow(w, host, race, raceMs, inst) {
  const x = race > 1 ? `_r${race}` : '';
  if (race > 1) w(`+0    log === race ${race} ===`);
  if (host) {
    w(`+0    dump lobby${x}`);
    w('+0    log create-game');
    w('+0    tap RETURN');
    w('+0    waitfng UI_QRModeSelect.fng 900');
    w('+200  log mode');
    for (let k = 0; k < (MODES[MODE] ?? 0); k++) w('+40   tap RIGHT');
    w(`+60   dump mode${x}`);
    w('+0    tap RETURN');
    w('+0    waitfng UI_QRTrackSelect.fng 900');
    w(`+200  dump track${x}`);
    w('+0    tap RETURN');
    w('+0    waitfng UI_QRModeOptions.fng 900');
    // Laps as they come (2, runtime/netrace.c); --ranked: Game Type (three rows down, drift two: it has no Track
    // Direction) Unranked -> Ranked, once (the screen keeps the last game's options)
    if (RANKED && race === 1) {
      w('+200  tap DOWN'); w('+40   tap DOWN'); if (MODE !== 'drift') w('+40   tap DOWN'); w('+40   tap RIGHT');
    }
    w(`+200  dump options${x}`);
    w('+0    tap RETURN');
    w(`+300  dump enter_room${x}`);
    w('+0    tap RETURN');                   // "Entering a game room may cause you to lose ... messages": OK
    w('+0    waitfng PC_OL_GameRoom.fng 900');
    w(`+300  dump room_0${x}`);
    w(`+${WAIT_ROOM}ms dump room_1${x}`);
    w('+0    log start-race');
    w('+0    click 0.311 0.172');            // Start Race
    w(`+3000ms dump start${x}`);
  } else {
    w(`+${race > 1 ? JOIN_WAIT + 15000 : JOIN_WAIT}ms dump lobby${x}`);
    if (RANKED && race === 1) {
      // the Filter asks for ranked games of the race's mode (the lobby lists them first): Filter (the 4th button),
      // Game Mode (Create Game's order), Game Type (two rows down) -> Ranked; leaving the Filter searches again, with
      // the focus back on Create Game
      w('+0    log filter-ranked');
      w('+0    tap RIGHT'); w('+40   tap RIGHT'); w('+40   tap RIGHT');
      w('+60   tap RETURN');
      w('+300  log filter-mode');
      for (let k = 0; k < (MODES[MODE] ?? 0); k++) w('+40   tap RIGHT');
      w('+40   tap DOWN'); w('+40   tap DOWN'); w('+40   tap RIGHT');
      w(`+60   dump filter${x}`);
      w('+0    tap RETURN');
      w(`+3000ms dump filtered${x}`);
      w('+0    log join-game');
      w('+0    tap RIGHT');
    } else {
      w('+0    log refresh');
      w('+0    tap RIGHT');                   // Refresh is the third button, Join Game the second
      w('+40   tap RIGHT');
      w('+60   tap RETURN');
      w(`+3000ms dump refreshed${x}`);
      w('+0    log join-game');
      w('+0    tap LEFT');
    }
    w('+60   tap RETURN');
    w(`+300  dump join_0${x}`);
    w('+0    tap RETURN');                   // the "Entering a game room" question, if it comes
    w('+0    waitfng PC_OL_GameRoom.fng 900');
    w(`+300  dump room_0${x}`);
    w('+0    log chat');
    w('+0    click 0.205 0.847');            // Chat Here
    w('+60   type hello from ${NFSU2_OL_NAME}');
    w('+60   tap RETURN');
    w(`+180  dump room_chat${x}`);
    w('+0    log ready');
    w('+0    click 0.311 0.172');            // Ready
    w(`+3000ms dump room_ready${x}`);
  }
  // the race: GameFlow 6, then the game's AI driver (drag: the synthetic pad)
  // (a joiner is Ready up to the host's whole room wait before the race: frames at 60 fps)
  w(`+0    waitmem 0x008654a4 6 ${Math.max(20000, Math.ceil((WAIT_ROOM / 1000 + 60) * 60))}`);
  w('+0    log race-loaded');
  // every player's car in the race (the coordinator's racer count), and the car memory pool as it goes
  w(`+0    waitexpr i:[0x00890118]+0x24 == ${PLAYERS} 1200`);
  w(`+0    expect i:[0x00890118]+0x24 == ${PLAYERS}`);
  w('+0    trace 600 racers=i:[0x00890118]+0x24 carpool_used=i:[0x00828164]+0x28 carpool_size=i:[0x00828164]+0x24');
  if (MODE === 'drag') {
    w('+0    trigger RT 1'); w('+0    wzero'); w('+9000ms log race-start');
    for (let k = 0; k < 16; k++) w('+1200ms tappad A');
  } else {
    w('+0    autopilot on 6000'); w('+0    wzero'); w('+0    log race-start');
  }
  if (HORN && race === 1) { w(`+${3000 + 4000 * (inst - 1)}ms log horn`); w('+0    down N'); w('+1000ms up N'); }
  if (QUIT_MID[0] === inst) {
    // this player's process ends in the middle of the race (sockets closed, nothing more sent)
    w(`+${QUIT_MID[1]}ms dump quit_mid`);
    w('+0    log quit-mid-race');
    w('+0    quit');
    return false;
  }
  for (let t = 0; t < raceMs; t += 20000) w(`+${Math.min(20000, raceMs - t)}ms dump race_${Math.min(raceMs, t + 20000) / 1000}${x}`);
  // the results: a ranked race shows the tournament results screen instead
  w(`+0    waitfng ${RANKED ? 'UI_PostRace_TournResults.fng' : 'UI_PostRaceResults.fng'} 30000`);
  w(`+0    log results${x}`);
  w('+0    untrace');
  w(`+180  dump results${x}`);
  w(`+3000ms dump results_2${x}`);
  if (RANKED) {
    // the ranked table: the server's $rfn, once two reports agree, sets the race handler's "final" byte (SPEED2.EXE
    // 0x58e05d); the screen reported on its own when it opened
    w('+0    waitexpr b:0x0086a6c0 == 1 6000');
    w(`+0    log results-table${x}`);
    w(`+60   dump results_table${x}`);
  }
  w('+300  tap RETURN');                     // Continue (unranked: the game logs in again, reports), the lobby
  w('+0    waitfng PC_OL_Lobby.fng 1800');
  w(`+3000ms dump after_results${x}`);
  return true;
}

// the suite's environment (web/tools/run-suite.mjs)
const COMMON = {
  NFSU2_SAVEDIR: '/save', NFSU2_READONLY_DATA: '1', NFSU2_DISCORD: '0', NOFOCUS: '2', NFSU2_FNG_LOG: '1',
  NFSU2_STATUS_EVERY: '600', NOMOVIES: '1', SNDSYNC: '0', GPUVS: '1', NFSU2_OL_NAME: '', NFSU2_OL_PASS: PASSWORD,
};
for (const kv of opts('env')) COMMON[kv.slice(0, kv.indexOf('='))] = kv.slice(kv.indexOf('=') + 1);
const stamp = Date.now().toString(36);
// the scripts are answered by this tool (Playwright's request interception) at a path of the page's own
// origin (the server's Content-Security-Policy refuses data: URLs; it serves only the repository's tests/)
const scriptPath = i => `/tests/online-pair/${stamp}-i${i + 1}.txt`;
function url(i) {
  const env = { ...COMMON, NFSU2_OL_NAME: NAMES[i], ...(EA_LOGIN ? {} : { NFSU2_ROAM_NAME: NAMES[i] }) };
  if (HIDE?.n === i + 1) env.NOFOCUS = '0';        // the game hears the page go to the background
  for (const kv of opts(`env-i${i + 1}`)) env[kv.slice(0, kv.indexOf('='))] = kv.slice(kv.indexOf('=') + 1);
  const q = new URLSearchParams({ wasm: '/game.wasm', pages: '16384', arg: G.exe,
    reg: G.reg, save: `ol-${stamp}-i${i + 1}`, net: NAMES[i], script: scriptPath(i) });
  if (PAGE === 'app') {
    // the player's page: no wasm/pages/arg/net (the app's own), the switch name from roam_name, free roam off
    const a = new URLSearchParams({ reg: q.get('reg'), save: q.get('save'), script: q.get('script') });
    for (const [k, v] of Object.entries({ ...env, NFSU2_ROAM: '0' })) if (k !== 'NFSU2_SAVEDIR') a.append('env', `${k}=${v}`);
    return `${BASE}/?${a}&fresh`;
  }
  const page = PAGE === 'play' && (!PLAY_ONLY || PLAY_ONLY === i + 1) ? 'play' : 'hostcheck';
  if (page === 'play') { q.set('w', String(SIZE[0])); q.set('h', String(SIZE[1])); q.set('artifacts', `/artifacts/i${i + 1}-`); q.set('dumpscale', '1'); }
  for (const [k, v] of Object.entries(env)) q.append('env', `${k}=${v}`);
  return `${BASE}/test/${page}.html?${q}&fresh`;
}

// our own server (stopped by its child handle only)
let server = null;
const serverLog = fs.createWriteStream(path.join(OUT, 'server.log'), { flags: 'a' });
async function startServer() {
  const c = spawn(...serverCommand(['--data', DATA, '--http-port', String(HTTP_PORT), '--port', '0',
    '--dev', '--artifacts', OUT, '--online-trace', path.join(OUT, 'trace.txt'), '--state', path.join(OUT, 'state'), ...opts('server-arg')]), { stdio: ['ignore', 'pipe', 'pipe'] });
  c.stdout.pipe(serverLog, { end: false }); c.stderr.pipe(serverLog, { end: false });
  await new Promise((res, rej) => {
    let out = '';
    const t = setTimeout(() => rej(new Error('server did not start')), 20000);
    c.stdout.on('data', d => { out += d; if (out.includes('this Mac') || out.includes('this host')) { clearTimeout(t); res(); } });
    c.on('exit', code => { clearTimeout(t); rej(new Error(`server exited (${code})`)); });
  });
  return c;
}
async function stopServer() {
  if (!server) return;
  const c = server; server = null;
  const gone = new Promise(r => c.once('exit', r));
  c.kill('SIGTERM'); await gone;
}
if (OWN) server = await startServer();

const CHROME_ARGS = ['--enable-unsafe-webgpu', ...(process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [])];
let browser, chrome = null;
if (HIDE) {
  // Chrome of our own, over CDP without Playwright's defaults: its focus emulation keeps every page it
  // made visible; a page of the default context without it goes to the background like a player's tab
  const prof = path.join(OUT, 'chrome-profile');
  fs.rmSync(prof, { recursive: true, force: true });
  const exe = process.env.CHROME_PATH || (process.platform === 'darwin'
    ? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome' : '/opt/google/chrome/chrome');
  chrome = spawn(exe, [...(argv.includes('--headed') ? [] : ['--headless=new']), '--remote-debugging-port=0', `--user-data-dir=${prof}`,
    '--no-first-run', '--no-default-browser-check', '--use-mock-keychain', '--password-store=basic', ...CHROME_ARGS, 'about:blank'],
  { stdio: ['ignore', 'ignore', 'pipe'] });
  const ws = await new Promise((res, rej) => {
    let err = '';
    const t = setTimeout(() => rej(new Error(`Chrome did not start: ${err}`)), 20000);
    chrome.stderr.on('data', d => { err += d; const m = /DevTools listening on (ws:\S+)/.exec(err); if (m) { clearTimeout(t); res(m[1]); } });
    chrome.on('exit', code => { clearTimeout(t); rej(new Error(`Chrome exited (${code}): ${err}`)); });
  });
  browser = await chromium.connectOverCDP(ws, { noDefaults: true });
} else browser = await chromium.launch({ channel: 'chrome', headless: !argv.includes('--headed'), args: CHROME_ARGS });
const t0 = Date.now();
const inst = [];
async function start(i) {
  const dir = path.join(OUT, `i${i + 1}`); fs.mkdirSync(dir, { recursive: true });
  const hide = HIDE?.n === i + 1 ? HIDE : null;
  const ctx = hide ? browser.contexts()[0] : await browser.newContext();
  const page = await ctx.newPage();
  await page.route(`**${scriptPath(i)}`, r => r.fulfill({ status: 200, contentType: 'text/plain; charset=utf-8', body: scripts[i] }));
  const plog = fs.createWriteStream(path.join(dir, 'page.log'));
  const lines = [];
  let exit = null;
  const w = (s) => plog.write(`${((Date.now() - t0) / 1000).toFixed(3)} ${s}\n`);
  // --hide: a blank tab in front of this one from "log FROM" to "log TO"; the times and what the page said
  const hidden = [];
  let bs = null, self = null, blank = null, queue = Promise.resolve();
  if (hide) {
    bs = await browser.newBrowserCDPSession();
    self = (await (await ctx.newCDPSession(page)).send('Target.getTargetInfo')).targetInfo.targetId;
  }
  const background = (on) => (queue = queue.then(async () => {
    if (on === !!blank) return;
    const at = (Date.now() - t0) / 1000;
    if (on) { blank = (await bs.send('Target.createTarget', { url: 'about:blank', newWindow: false })).targetId; await bs.send('Target.activateTarget', { targetId: blank }); }
    else { await bs.send('Target.activateTarget', { targetId: self }); await bs.send('Target.closeTarget', { targetId: blank }); blank = null; }
    await new Promise(r => setTimeout(r, 300));
    const state = await page.evaluate(() => document.visibilityState).catch(e => `? ${e}`);
    if (on) hidden.push({ at, state }); else Object.assign(hidden[hidden.length - 1] ?? {}, { back: at, backState: state });
    const s = `[online-pair] ${on ? 'another tab in front' : 'this tab in front again'} at ${at.toFixed(1)} s: the page is ${state}`;
    w(s); lines.push(s); console.log(`online-pair: ${NAMES[i]} ${s.slice(14)}`);
  }).catch(e => w(`[online-pair] tab switch failed: ${e}`)));
  const hideRe = hide && new RegExp(`\\bLOG ${hide.from}$`), showRe = hide && new RegExp(`\\bLOG ${hide.to}(_r\\d+)?$`);
  page.on('console', m => {
    const t = m.text(); w(t);
    const g = /^\[\d+!?\] ([^]*)$/.exec(t);
    if (g) lines.push(g[1]); else { const e = /^EXIT (-?\d+)$/.exec(t); if (e) exit = +e[1]; lines.push(t); }
    if (PAGE === 'app') { const x = /guest exited with code (-?\d+)|\[app\] the game exited with code (-?\d+)/.exec(t); if (x) exit = +(x[1] ?? x[2]); }
    if (hide && hideRe.test(t)) void background(true);
    if (hide && showRe.test(t)) void background(false);
    // the game's frame rate while in the background ([status] every NFSU2_STATUS_EVERY frames: none = parked)
    const fps = hide && blank && /\[status\] f=\d+ .*\bfps=([\d.]+)/.exec(t), cur = hidden[hidden.length - 1];
    if (fps && cur && cur.back === undefined) (cur.fps ??= []).push(+fps[1]);
  });
  page.on('pageerror', e => { w('PAGEERROR ' + e); lines.push(`PAGEERROR ${e}`); });
  page.on('crash', () => lines.push('CRASH (renderer process)'));
  const u = url(i);
  w(`URL ${u.replace(/NFSU2_OL_PASS%3D[^&]*/, 'NFSU2_OL_PASS%3D<redacted>')}`);
  await page.goto(u);
  if (PAGE === 'app') {
    await page.waitForFunction(() => window.__nfsu2?.phase === 'ready', null, { timeout: 120000 });
    await page.click('#play');
    w('[online-pair] clicked Play');
  }
  const timer = setInterval(async () => {
    try {
      const s = await page.evaluate(() => {
        const n = window.__nfsu2?.session?.net ?? globalThis.nfsu2Net;
        return n ? { ...n.stats(), peers: typeof n.peers === 'function' ? n.peers() : n.peers } : null;
      });
      if (s) w(`NETSTAT ${JSON.stringify(s)}`);
    } catch {}
  }, 10000);
  const r = { i, name: NAMES[i], dir, page, ctx, plog, lines, timer, hidden, exitCode: () => exit, done: null };
  r.done = page.waitForFunction(() => document.title === 'DONE' || window.__nfsu2?.phase === 'exited', null, { timeout: TMO, polling: 1000 })
    .then(() => 'done', () => 'timeout');
  inst.push(r);
  console.log(`online-pair: ${r.name} started at ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  return r;
}
for (let i = 0; i < PLAYERS; i++) { if (i) await new Promise(r => setTimeout(r, STAGGER)); await start(i); }
const res = await Promise.all(inst.map(x => x.done));
console.log(`online-pair: ${inst.map((x, k) => `${x.name} ${res[k]}`).join(', ')} after ${((Date.now() - t0) / 1000).toFixed(0)} s`);
for (const x of inst) {
  clearInterval(x.timer);
  if (res[x.i] === 'timeout') x.lines.push(`TIME LIMIT REACHED: online-pair stopped the run after ${(TMO / 1000).toFixed(0)} s`);
  fs.writeFileSync(path.join(x.dir, 'log.txt'), x.lines.join('\n') + '\n');
  x.plog.end();
}
await browser.close();
if (chrome) { const gone = new Promise(r => chrome.once('exit', r)); chrome.kill('SIGTERM'); await gone; }
await stopServer();
serverLog.end();

// judge
const flow = !FILES.length;
const summary = { players: PLAYERS, mode: MODE, races: RACES, flow: flow ? 'race' : FILES, result: {}, why: [] };
for (const x of inst) {
  const log = fs.readFileSync(path.join(x.dir, 'log.txt'), 'utf8'), has = (re) => re.test(log), n = `i${x.i + 1}`;
  const why = [];
  if (res[x.i] !== 'done') why.push(`${n}:timeout`);
  if (x.exitCode() !== 0) why.push(`${n}:exit=${x.exitCode()}`);
  if (!log.includes('quit at guest frame')) why.push(`${n}:no-quit`);
  if (flow) {
    if (!has(OL.main)) why.push(`${n}:no-online-main`);
    if (!log.includes(OL.room)) why.push(`${n}:no-game-room`);
    const quitter = QUIT_MID[0] === x.i + 1;
    if (!quitter && !has(OL.results)) why.push(`${n}:no-results`);
    if (quitter && !log.includes('LOG quit-mid-race')) why.push(`${n}:no-quit-mid`);
    if (RACES > 1 && !has(new RegExp(`LOG results_r${RACES}$`, 'm'))) why.push(`${n}:no-results-r${RACES}`);
  }
  if (has(/^\[expect\] FAIL/m)) why.push(`${n}:expect`);
  if (has(/FAULT IN GUEST|GUEST MEMORY FAULT|HOST SEGV/)) why.push(`${n}:fault`);
  if (log.includes('TIME LIMIT REACHED')) why.push(`${n}:timeout`);
  if (has(/^\[trap\]/m) || has(/^TRAP in thread/m)) why.push(`${n}:trap`);
  if (log.includes('UNRESOLVED INDIRECT')) why.push(`${n}:unresolved-icall`);
  if (has(/^\[net\] BLOCKED/m)) why.push(`${n}:net-blocked`);
  if (has(/^PAGEERROR|^CRASH/m)) why.push(`${n}:page-error`);
  if (HIDE?.n === x.i + 1) {
    // in the background the game was told (deactivated) and kept running in the network session
    if (!x.hidden.some(h => h.state === 'hidden')) why.push(`${n}:never-hidden`);
    if (!has(/^app: deactivated/m)) why.push(`${n}:not-deactivated`);
    if (x.hidden.some(h => (h.back ?? (Date.now() - t0) / 1000) - h.at >= 30 && !h.fps?.length)) why.push(`${n}:paused-in-background`);
  }
  const L = log.split('\n'), grab = (re) => L.filter(l => re.test(l));
  summary.result[x.name] = {
    exit: x.exitCode(), quit: grab(/quit at guest frame/).pop() ?? null,
    screens: [...new Set(grab(/^\[fng\] screen /).map(l => l.replace(/^\[fng\] screen /, '').split(' ')[0]))],
    online: grab(/^\[net\] (online|virtual LAN: tcp connect 10\.77\.0\.1)/).slice(0, 8),
    lanstats: grab(/^\[net\] virtual LAN (stats|at exit)/).slice(-1),
    ...(HIDE?.n === x.i + 1 ? { hidden: x.hidden, background: grab(/^app: (de)?activated|the game keeps running|^\[online-pair\]/) } : {}),
    why,
  };
  summary.why.push(...why);
}
if (argv.includes('--canary')) {
  // every file the run wrote except the scripts (which name the variable, not the password) and the frame dumps
  const files = [];
  const walk = d => { for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) walk(p); else if (!/\.png$/.test(e.name) && !/^i\d+\.txt$/.test(e.name)) files.push(p);
  } };
  walk(OUT);
  const hits = files.filter(f => fs.readFileSync(f).includes(PASSWORD));
  const trace = fs.existsSync(path.join(OUT, 'trace.txt')) ? fs.readFileSync(path.join(OUT, 'trace.txt'), 'utf8') : '';
  const passFields = [...trace.matchAll(/\bPASS\w*=(\S+)/g)].map(m => m[1]);
  const srv = fs.readFileSync(path.join(OUT, 'server.log'), 'utf8').split('\n').filter(l => /^\[(online|lan)\]/.test(l));
  const realAddr = srv.filter(l => [...l.matchAll(/\b(\d{1,3}(?:\.\d{1,3}){3})\b/g)].some(m => !/^10\.77\.[0-7]\./.test(m[1])) || /::|localhost/.test(l));
  summary.canary = { files: files.length, hits: hits.map(f => path.relative(OUT, f)), passFields: passFields.length,
    passShown: passFields.filter(v => v !== '<redacted>').length, serverLines: srv.length, realAddressLines: realAddr.length };
  if (!OWN) summary.why.push('canary:needs-own-server');
  if (hits.length) summary.why.push(`canary:password-in(${summary.canary.hits.join(',')})`);
  if (!passFields.length) summary.why.push('canary:no-PASS-seen (the login did not happen?)');
  if (summary.canary.passShown) summary.why.push('canary:PASS-not-redacted');
  if (realAddr.length) summary.why.push('canary:real-address-in-log');
}
// the server's view: every race's result was taken (the online host logs one line per race)
const srvLog = fs.readFileSync(path.join(OUT, 'server.log'), 'utf8');
summary.serverResults = srvLog.split('\n').filter(l => /^\[online\] ".*" \((circuit|sprint|drift|drag|streetx|url|type \d+), (un)?ranked\): /.test(l));
if (flow && OWN && summary.serverResults.length < RACES) summary.why.push(`server:results-${summary.serverResults.length}-of-${RACES}`);
summary.pass = summary.why.length === 0;
fs.writeFileSync(path.join(OUT, 'summary.json'), JSON.stringify(summary, null, 2));
console.log(JSON.stringify(summary, null, 2));
console.log(summary.pass ? `PASS online-pair (${OUT})` : `FAIL online-pair: ${summary.why.join(' ')} (${OUT})`);
process.exitCode = summary.pass ? 0 : 1;
