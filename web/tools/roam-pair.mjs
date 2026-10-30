// Free-roam multiplayer between two browsers: two game instances in two
// headless Chrome pages (separate browser contexts, so separate OPFS) against
// one local server, both with roam on, talking through the server's switch.
//
//   node web/tools/roam-pair.mjs --out <dir> [--mode vlan|loop] [--port 8516 --http-port 8616]
//        [--timeout 1200] [--drive 3600] [--watch 2400] [--stagger 2] [--bounce <s>]
//        [--a Alice:roam_mp_drive.txt] [--b Bob:roam_mp_watch.txt] [--c Carol:<script>] [--d Dave:<script>] [--solo]
//        [--env K=V ...] [--headed]
//        (an instance's own settings after its script: --a Alice:roam_mp_ram_a.txt:NFSU2_ROAM_COLLIDE=0,K=V; a comma
//        not followed by a K= stays in the value: NFSU2_ROAM_CREW=Blue Room, Night Owls)
//        [--base http://localhost:8616]   (use a server that is already running instead)
//        [--https]   the pages load from https://localhost:<port> (the switch over wss://): our own server
//                    gets a test certificate (web/test/cf-sim.mjs makeTestCerts, --tls-cert), whose
//                    errors the pages ignore instead of trusting it
//        [--page app]  each instance is the player's start page (web/index.html): Play is clicked, the
//                    renderer, audio, input and network are the app's (free roam from the page address's
//                    NFSU2_ROAM=1); the default is test/hostcheck.html (null renderer, net=<name>).  A page of
//                    1600x900 (--size), and a script line `log SHOT <name>` saves the page as the player sees it
//                    (the game and the page's own overlays) as <out>/<instance>-<name>.png; `log SHOT clean_<name>`
//                    without the race lobby's panel (tests/scripts/mprace_shots_a/b.txt); `log MENU <item>` opens
//                    the page menu (F9), clicks that item (its data-act, e.g. solid) and goes back to the game;
//                    `log KEY <code> ...` presses those keys on the page as the player does (KeyboardEvent.code: the
//                    script's own keys reach only the game, never the page) and `log STICK <x> <y>` moves the right
//                    stick there for the page (quick chat's picker: tests/scripts/roam_say_view_a.txt); with
//                    --touch the pages take taps (the touch controls show), and `log TAP <selector>` taps there
//                    (tests/scripts/roam_say_touch.txt)
//                    `log CREW join <name>` / `log CREW leave <name>` / `log CREW colour <k> <name>` (the palette's
//                    k-th colour, from 0) do that in the menu's Crews popup (F9, Crews) and go back (Done, Back to
//                    the game), `log CREW shot <name>` saves the page with the popup open
//                    (tests/scripts/roam_crew_menu.txt); `log INVITE <fragment>` opens that invite link in the page
//                    as a link pasted into its address does (the fragment without its #, which starts a comment in a
//                    script: room=Blue%20Room), saves the question as <out>/<instance>-invite-<k>.png (k from 1) and
//                    presses Join; `log INVITE-NO <fragment>` presses Not now (tests/scripts/roam_invite.txt)
//        [--page play [--size 960x540] [--play-only Bob]]   the renderer's test page (test/play.html, WebGPU)
//                    instead: frame dumps go to the server's /artifacts/ as <name>-*.png (a server of our own
//                    gets --artifacts <out>); --play-only: only that instance renders, the others run hostcheck
//        [--page-arg bc=0]  more of the page address (each page's, repeated for several): play.html's bc=0 takes a
//                    phone's texture path (DXT1 as ASTC, the rest unpacked) on this Mac
//        [--download-log]  with --page app: at the end each page's Download log, as a player gets it (privacy.ts:
//                    no names or addresses), as <out>/<name>.download.log
//        [--fetch snd.raw]  a file of each page's save folder (OPFS) copied out as <out>/<name>.<file>,
//                    e.g. the sound output of --env SNDDUMP=/save/snd.raw (s16le stereo, 44.1 kHz)
//        [--speedhack Cat:3]  that page's game threads (each game-worker.js, as it loads) see performance.now and
//                    Date.now run 3 times as fast, as a speed hack in the browser makes them: the whole game runs
//                    faster (the server's cheat watch sees it: tests/scripts/roam_cheat_c.txt)
//        [--hide Bob:60:8]  that page reports itself hidden (document.hidden + visibilitychange, as when
//                    its tab goes to the background; headless pages are never really hidden) at 60 s
//                    after the first page opened, for 8 s
//        [--crowd <n>]  web/tools/roam-crowd.mjs's n simulated players on the same server for the whole run
//                    (<out>/crowd.log): with 5 or more friends the first cars of our own come into use, in the AI range
//        [--crowd-args "--cars all"]  more of roam-crowd's options, as one argument
//        [--crowd-race 1-3:200:320]  roam-crowd's --race: those players are in a race meanwhile
//        [--seed <save tree>]  every page starts from that save (web/tools/save-seed.mjs, as run-suite's
//                    NFSU2_SAVE_SEED: e.g. a player's "Download a backup" unzipped) instead of a fresh one
//        [--fresh Neo]  that page starts from a fresh profile all the same (a new career's prologue beside
//                    --seed's careers: tests/scripts/mprace_nocar_b.txt)
//        [--seed-for Bob:<save tree>]  that page starts from its own save instead (two careers' cars in one
//                    race: mprace_lobby_a.txt + mprace_card_b.txt at a race open in its career)
//        [--game ug2|mw]   the game (ug2 unless given): its pair of scripts for --a and --b; Most Wanted has none
//                    yet
//        [--help]    this text and the game's pair
//
// Without --base it starts its own server (web/tools/server-cmd.mjs, --dev on the given
// ports, the game folder from --data or NFSU2_DATA) and stops it at the end.
// --bounce <s>: stops that server <s> seconds after the first page opened and
// starts it again 3 s later (the pages reconnect to the switch).
//
// mode vlan: the players' path (NFSU2_ROAM_WEBTEST=vlan): virtual addresses,
//            discovery by broadcast to the switch, no configured peers.
// mode loop: the test harness (as tools/roam_multi.sh on the Mac): each its own
//            roam_port (47300 + i), peers 127.0.0.1:<the other's port>, the
//            switch as the emulated loopback.
// 2 to 4 instances (--c, --d add a third and a fourth; --solo: --a only, with
// web/tools/roam-crowd.mjs's players on the same server).  Writes <out>/<name>.log
// (the page console), <name>.pose.csv (the pose log from the page's OPFS) and
// <out>/summary.json; then runs tools/roam_compare.py for the first (the driver)
// against every other instance.
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync, spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { makeTestCerts } from '../test/cf-sim.mjs';
import { serverCommand } from './server-cmd.mjs';
import { seedSave } from './save-seed.mjs';
import { GAMES, gameOf, usage, nothingFor, requireGame } from './rig-game.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d; };
const opts = (k) => argv.flatMap((a, i) => (a === '--' + k && i + 1 < argv.length ? [argv[i + 1]] : []));
// each game's pair (name:script of tests/scripts/): the driver (--a) and the one who watches it (--b)
const PAIRS = { ug2: { a: 'Alice:roam_mp_drive.txt', b: 'Bob:roam_mp_watch.txt' } };
const NONE = 'none yet: no free-roam scripts for Most Wanted (M10-25)';
const GAME = gameOf(argv), G = GAMES[GAME], PAIR = PAIRS[GAME];
if (argv.includes('--help')) usage(import.meta.url, GAME, 'The pair', PAIR ? [`--a ${PAIR.a} (drives)`, `--b ${PAIR.b} (watches)`] : [], NONE);
if (!PAIR) nothingFor(GAME, NONE);
const HTTP_PORT = +opt('http-port', 8616), HTTPS_PORT = +opt('port', 8516);
const OWN = !argv.includes('--base');
const HTTPS = argv.includes('--https');
const BASE = opt('base', HTTPS ? `https://localhost:${HTTPS_PORT}` : `http://localhost:${HTTP_PORT}`);
if (!OWN) await requireGame(BASE, GAME);
const DATA = opt('data', process.env.NFSU2_DATA || path.join(os.homedir(), 'projects/nsfu2/Need for Speed - Underground 2'));
const BOUNCE = argv.includes('--bounce') ? +opt('bounce', 0) * 1000 : 0;
const OUT = path.resolve(opt('out', path.join(REPO, 'out/web/roam-pair')));
const MODE = opt('mode', 'vlan');
const TMO = +opt('timeout', 1200) * 1000;
const STAGGER = +opt('stagger', 2) * 1000;
const SPECS = (argv.includes('--solo') ? [opt('a', PAIR.a)]
  : [opt('a', PAIR.a), opt('b', PAIR.b), opt('c', null), opt('d', null)])
  .filter(Boolean).map(x => x.split(':'));
const N = SPECS.length;
const APP = opt('page', 'test') === 'app';
const DOWNLOAD_LOG = APP && argv.includes('--download-log');
const PLAY = opt('page', 'test') === 'play', SIZE = opt('size', opt('page', 'test') === 'app' ? '1600x900' : '960x540').split('x').map(Number), PLAY_ONLY = opt('play-only', '');
const FETCH = opts('fetch');                 // files of each page's save folder to copy out, as <out>/<name>.<file>
const SEED = opt('seed', '');               // a save tree each page starts from
const FRESH = opts('fresh');                 // pages that start from a fresh profile all the same
const SEED_FOR = new Map(opts('seed-for').map(s => [s.slice(0, s.indexOf(':')), s.slice(s.indexOf(':') + 1)]));   // a page's own
const seedOf = name => SEED_FOR.get(name) || (FRESH.includes(name) ? '' : SEED);
const seeded = name => !!seedOf(name);
const SPEEDHACK = new Map(opts('speedhack').map(x => [x.split(':')[0], +(x.split(':')[1] ?? 3)]));
const HIDE = opts('hide').map(x => { const [n, at, dur] = x.split(':'); return { name: n, at: +at * 1000, dur: +(dur ?? 8) * 1000 }; });
const TOUCH = argv.includes('--touch');      // the pages take taps (`log TAP <selector>`): the touch controls show
fs.mkdirSync(OUT, { recursive: true });

const COMMON = {
  NFSU2_SAVEDIR: '/save', NFSU2_READONLY_DATA: '1', NOMOVIES: '1', SNDSYNC: '0', GPUVS: '1', NFSU2_FNG_LOG: '1',
  NFSU2_STATUS_EVERY: '1200', NFSU2_DISCORD: '0', NFSU2_ROAM: '1', NFSU2_ROAM_POSELOG: '/save/pose.csv',
  ROAM_DRIVE_FRAMES: opt('drive', '3600'), ROAM_WATCH_FRAMES: opt('watch', '2400'),
  // the pairs' scripts start both players on the new career's start, car inside car, and see each other there: the
  // game hides cars on that spot (roam_game.c at_spawn) unless told otherwise (--env NFSU2_ROAM_SPAWN_M=5 tests that)
  NFSU2_ROAM_SPAWN_M: '0',
};
for (const kv of opts('env')) COMMON[kv.slice(0, kv.indexOf('='))] = kv.slice(kv.indexOf('=') + 1);

function url(i, name, script, own) {
  const env = { ...COMMON, NFSU2_ROAM_NAME: name, ROAM_PROFILE: name };
  for (const kv of own ? own.split(/,(?=[A-Za-z0-9_]+=)/) : []) env[kv.slice(0, kv.indexOf('='))] = kv.slice(kv.indexOf('=') + 1);
  if (MODE === 'vlan') env.NFSU2_ROAM_WEBTEST = 'vlan';
  else {
    env.NFSU2_ROAM_PORT = String(47300 + i);
    env.NFSU2_ROAM_PEERS = [...Array(N).keys()].filter(j => j !== i).map(j => `127.0.0.1:${47300 + j}`).join(',');
  }
  const q = APP
    ? new URLSearchParams({ reg: G.reg, script: `/tests/scripts/${script}`, save: `roam-${name}` })
    : new URLSearchParams({ wasm: '/game.wasm', pages: '16384', arg: G.exe,
      reg: G.reg, script: `/tests/scripts/${script}`, save: `roam-${name}`, net: name });
  const play = PLAY && (!PLAY_ONLY || PLAY_ONLY === name);
  if (play) { q.set('w', String(SIZE[0])); q.set('h', String(SIZE[1])); q.set('artifacts', `/artifacts/${name}-`); q.set('dumpscale', '1'); }
  for (const [k, v] of Object.entries(env)) if (!(APP && (k === 'NFSU2_SAVEDIR'))) q.append('env', `${k}=${v}`);
  for (const kv of opts('page-arg')) q.append(kv.slice(0, kv.indexOf('=')), kv.slice(kv.indexOf('=') + 1));
  const fresh = seeded(name) ? '' : '&fresh';
  return APP ? `${BASE}/?${q}${fresh}` : `${BASE}/test/${play ? 'play' : 'hostcheck'}.html?${q}${fresh}`;
}

// our own server (stopped by its child handle only)
let server = null;
const serverLog = fs.createWriteStream(path.join(OUT, 'server.log'), { flags: 'a' });
async function startServer() {
  const certs = HTTPS ? makeTestCerts(path.join(OUT, 'certs')) : null;
  const c = spawn(...serverCommand(['--data', DATA,
    ...(certs ? ['--port', String(HTTPS_PORT), '--tls-cert', certs.cert, '--tls-key', certs.key] : []),
    '--http-port', String(HTTP_PORT), '--dev', ...(PLAY ? ['--artifacts', OUT] : [])]), { stdio: ['ignore', 'pipe', 'pipe'] });
  c.stdout.pipe(serverLog, { end: false }); c.stderr.pipe(serverLog, { end: false });
  await new Promise((res, rej) => {
    let out = '';
    const t = setTimeout(() => rej(new Error('server did not start')), 20000);
    c.stdout.on('data', d => { out += d; if (out.includes('  access    :')) { clearTimeout(t); res(); } });
    c.on('exit', code => { clearTimeout(t); rej(new Error(`server exited (${code})`)); });
  });
  serverLog.write(`--- server pid ${c.pid} up at ${new Date().toISOString()}\n`);
  return c;
}
async function stopServer() {
  if (!server) return;
  const c = server; server = null;
  const gone = new Promise(r => c.once('exit', r));
  c.kill('SIGTERM'); await gone;
  serverLog.write(`--- server stopped at ${new Date().toISOString()}\n`);
}
if (OWN) server = await startServer();
const CROWD = +opt('crowd', 0);
let crowd = null;
if (CROWD) {
  const log = fs.openSync(path.join(OUT, 'crowd.log'), 'w');
  crowd = spawn(process.execPath, [path.join(REPO, 'web/tools/roam-crowd.mjs'), '--base', BASE, '--n', String(CROWD), '--seconds', String(TMO / 1000),
    ...(opt('crowd-race', '') ? ['--race', opt('crowd-race', '')] : []), ...opt('crowd-args', '').split(' ').filter(Boolean)],
    { cwd: REPO, stdio: ['ignore', log, log] });
}

// CHROME_ARGS: more of the browser's options, separated by spaces (inside WSL: the Vulkan ones of online-pair's note)
const browser = await chromium.launch({ ...pickBrowser(), headless: !argv.includes('--headed'), args: process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [] });
const t0 = Date.now();
const inst = [];
async function start(i, name, script, own) {
  const ctx = await browser.newContext({ ...(HTTPS ? { ignoreHTTPSErrors: true } : {}), ...(APP ? { viewport: { width: SIZE[0], height: SIZE[1] } } : {}),
    ...(TOUCH ? { hasTouch: true } : {}), ...(DOWNLOAD_LOG ? { acceptDownloads: true } : {}) });
  const page = await ctx.newPage();
  const log = fs.createWriteStream(path.join(OUT, `${name}.log`));
  let invites = 0;                                      // (the page's invite links so far: log INVITE)
  const w = (s) => log.write(`${((Date.now() - t0) / 1000).toFixed(3)} ${s}\n`);
  page.on('console', m => {
    w(m.text());
    // the app page as the player sees it, at a script's `log SHOT <name>` (clean_*: without the lobby's panel)
    const k = APP && /LOG SHOT (\S+)/.exec(m.text());
    if (k) void (k[1].startsWith('clean') ? page.evaluate(() => { for (const id of ['lobby', 'lobby-cover']) document.getElementById(id).hidden = true; }) : Promise.resolve())
      .then(() => page.screenshot({ path: path.join(OUT, `${name}-${k[1]}.png`) }))
      .then(() => w(`[shot] ${k[1]}`), e => w(`[shot] ${k[1]} failed: ${e}`));
    // the page menu as the player uses it, at a script's `log MENU <item>` (its data-act: solid, fps ...): F9, the
    // item, back to the game
    const mk = APP && /LOG MENU (\S+)/.exec(m.text());
    if (mk) void page.keyboard.press('F9').then(() => page.click(`#menu [data-act="${mk[1]}"]`, { timeout: 5000 }))
      .then(() => page.click('#menu [data-act="resume"]'))
      .then(() => w(`[menu] ${mk[1]}`), e => w(`[menu] ${mk[1]} failed: ${e}`));
    // keys pressed on the page, and the right stick as the page reads it, at a script's `log KEY <code> ...` and
    // `log STICK <x> <y>`
    const kk = APP && /LOG KEY ([\w ]+)/.exec(m.text());
    if (kk) void kk[1].trim().split(/\s+/).reduce((p, c) => p.then(() => page.keyboard.press(c)), Promise.resolve())
      .then(() => w(`[key] ${kk[1].trim()}`), e => w(`[key] ${kk[1]} failed: ${e}`));
    const sk = APP && /LOG STICK (-?[\d.]+) (-?[\d.]+)/.exec(m.text());
    if (sk) void page.evaluate(([x, y]) => window.__nfsu2.quickChatStick(x, y), [+sk[1], +sk[2]])
      .then(r => w(`[stick] ${sk[1]} ${sk[2]}: ${r ? 'the picker' : 'not used'}`), e => w(`[stick] failed: ${e}`));
    // a finger's tap on the page (--touch), at a script's `log TAP <selector>`
    const tk = APP && TOUCH && /LOG TAP (.+)$/.exec(m.text());
    if (tk) void page.tap(tk[1].trim(), { timeout: 5000 }).then(() => w(`[tap] ${tk[1].trim()}`), e => w(`[tap] ${tk[1]} failed: ${e}`));
    // the menu's Crews popup as the player uses it, at a script's `log CREW <op> ...`: F9, Crews, the act in the popup,
    // Done, Back to the game
    const ck = APP && /LOG CREW (join|leave|colour|shot) (?:(\d+) )?(.+)$/.exec(m.text());
    if (ck) {
      const [, op, k, arg] = ck, pop = '#crews-pop', crew = () => page.locator(`${pop} li.crew`, { has: page.locator('.crew-name', { hasText: arg.trim() }) });
      const act = op === 'join' ? () => page.fill('#crews-pop-input', arg.trim()).then(() => page.press('#crews-pop-input', 'Enter'))
        : op === 'leave' ? () => crew().locator('.crew-leave').click({ timeout: 5000 })
        : op === 'colour' ? () => crew().locator('.crew-dot').click({ timeout: 5000 }).then(() => page.locator(`${pop} .crew-colours .swatch`).nth(+k).click({ timeout: 5000 }))
        : () => page.waitForTimeout(300).then(() => page.screenshot({ path: path.join(OUT, `${name}-${arg.trim()}.png`) }));
      void page.keyboard.press('F9').then(() => page.click('#menu [data-act="crews"]', { timeout: 5000 }))
        .then(() => page.waitForSelector(`${pop}[open]`, { timeout: 5000 })).then(act)
        .then(() => page.waitForTimeout(200)).then(() => page.click(`${pop} [data-act="crews-done"]`))
        .then(() => page.click('#menu [data-act="resume"]'))
        .then(() => w(`[crew] ${op} ${k ?? ''} ${arg}`), e => w(`[crew] ${op} ${arg} failed: ${e}`));
    }
    // an invite link opened in the page, at a script's `log INVITE <fragment>`: the question box, its picture and
    // what it says, then Join (or Not now)
    const ik = APP && /LOG INVITE(-NO)? (\S+)/.exec(m.text());
    if (ik) {
      const [, no, frag] = ik, shot = path.join(OUT, `${name}-invite-${++invites}.png`);
      void page.evaluate(f => { location.hash = '#' + f; }, frag)
        .then(() => page.waitForSelector('#confirm[open]', { timeout: 10000 }))
        .then(() => page.waitForTimeout(300)).then(() => page.screenshot({ path: shot }))
        .then(() => page.evaluate(() => `${document.getElementById('confirm-title').textContent} | ${document.getElementById('confirm-body').innerText.replace(/\s*\n+\s*/g, ' / ')}`))
        .then(said => { w(`[invite] ${frag}: ${said}`); return page.click(no ? '#confirm-cancel' : '#confirm-ok'); })
        .then(() => page.waitForTimeout(500)).then(() => page.evaluate(() => [document.getElementById('toast').textContent, document.getElementById('page-toast').textContent].filter(Boolean).join(' | ')))
        .then(told => w(`[invite] ${no ? 'not now' : 'joined'}: ${told}`), e => w(`[invite] ${frag} failed: ${e}`));
    }
  });
  page.on('pageerror', e => w('PAGEERROR ' + e));
  // a speed hack: the game threads' clocks k times as fast from their start (a worker's own loop never yields to
  // anything sent to it later, so the hack goes in with its script, as a browser extension's does)
  const k = SPEEDHACK.get(name);
  if (k) await ctx.route(r => r.pathname.endsWith('/host/game-worker.js'), async (route) => {
    const res = await route.fetch();
    const hook = `{ const p = performance, real = p.now.bind(p), rd = Date.now, d0 = rd(); p.now = () => real() * ${k}; Date.now = () => d0 + (rd() - d0) * ${k}; }\n`;
    w(`[speedhack] ${route.request().url()}: its clocks ${k} times as fast`);
    await route.fulfill({ response: res, body: hook + await res.text() });
  });
  const u = url(i, name, script, own);
  if (seeded(name)) w(`[roam-pair] save seed: ${await seedSave(page, BASE, `roam-${name}`, seedOf(name))} files from ${seedOf(name)}`);
  w(`URL ${u}`);
  await page.goto(u);
  if (APP) {
    // the player's page: Ready, then a trusted click on Play
    await page.waitForFunction(() => window.__nfsu2?.phase === 'ready', null, { timeout: 120000 });
    await page.click('#play');
    w('[roam-pair] clicked Play');
  }
  // the page's view of the virtual LAN every 10 s
  const timer = setInterval(async () => {
    try {
      const s = await page.evaluate(() => {
        if (window.__nfsu2?.session) { const n = window.__nfsu2.session.net; return n ? { ...n.stats(), vip: n.vip(), peers: n.peers() } : { net: 'not attached' }; }
        const n = globalThis.nfsu2Net; return n ? { ...n.stats(), peers: n.peers } : null;
      });
      if (s) w(`NETSTAT ${JSON.stringify(s)}`);
      // the app page: what its copy of the log for a stopped page says the game held (lastgame.ts)
      const held = APP && await page.evaluate(() => {
        for (let i = 0; i < localStorage.length; i++) {
          const k = localStorage.key(i);
          if (k?.startsWith('nfsu2.lastgame.')) return JSON.parse(localStorage.getItem(k) ?? 'null')?.held ?? '';
        }
        return null;
      });
      if (held) w(`HELD ${held}`);
    } catch {}
  }, 10000);
  const r = { name, page, ctx, log, w, timer, done: null };
  r.done = page.waitForFunction(APP ? () => window.__nfsu2?.phase === 'exited' : () => document.title === 'DONE', null, { timeout: TMO, polling: 1000 })
    .then(() => 'done', () => 'timeout');
  if (APP) r.done = r.done.then(async (x) => { w(`EXIT ${await page.evaluate(() => window.__nfsu2.lastExit?.code).catch(() => '?')}`); return x; });
  for (const h of HIDE.filter(h => h.name === name)) {
    const hide = (on) => page.evaluate((on) => {
      Object.defineProperty(document, 'hidden', { configurable: true, get: () => on });
      Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => (on ? 'hidden' : 'visible') });
      document.dispatchEvent(new Event('visibilitychange'));
    }, on).catch(() => {});
    setTimeout(() => { w(`[roam-pair] page reports hidden for ${h.dur / 1000} s`); void hide(true); }, Math.max(0, h.at - (Date.now() - t0)));
    setTimeout(() => { w('[roam-pair] page reports visible again'); void hide(false); }, Math.max(0, h.at + h.dur - (Date.now() - t0)));
  }
  inst.push(r);
  return r;
}
await start(0, ...SPECS[0]);
let bounced = null;
if (BOUNCE && OWN) bounced = new Promise(r => setTimeout(async () => {
  console.log(`roam-pair: stopping the server at ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  await stopServer();
  await new Promise(q => setTimeout(q, 3000));
  server = await startServer();
  console.log(`roam-pair: server back at ${((Date.now() - t0) / 1000).toFixed(1)} s`);
  r();
}, BOUNCE));
for (let i = 1; i < N; i++) {
  await new Promise(r => setTimeout(r, STAGGER));
  await start(i, ...SPECS[i]);
}
const res = await Promise.all(inst.map(x => x.done));
console.log(`roam-pair: ${inst.map((x, i) => `${x.name} ${res[i]}`).join(', ')} after ${((Date.now() - t0) / 1000).toFixed(0)} s`);

// the pose logs from each page's OPFS
for (const x of inst) {
  clearInterval(x.timer);
  try {
    const txt = await x.page.evaluate(async (dir) => {
      const root = await navigator.storage.getDirectory();
      const d = await root.getDirectoryHandle(dir);
      const f = await (await d.getFileHandle('pose.csv')).getFile();
      return await f.text();
    }, `roam-${x.name}`);
    fs.writeFileSync(path.join(OUT, `${x.name}.pose.csv`), txt);
    console.log(`roam-pair: ${x.name}.pose.csv ${txt.split('\n').length - 1} rows`);
  } catch (e) { console.log(`roam-pair: no pose log from ${x.name}: ${e}`); }
  for (const f of FETCH) {
    try {
      const b64 = await x.page.evaluate(async ([dir, name]) => {
        const parts = name.split('/');
        let d = await (await navigator.storage.getDirectory()).getDirectoryHandle(dir);
        for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p);
        const u8 = new Uint8Array(await (await (await d.getFileHandle(parts.at(-1))).getFile()).arrayBuffer());
        let s = ''; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode(...u8.subarray(i, i + 0x8000));
        return btoa(s);
      }, [`roam-${x.name}`, f]);
      const buf = Buffer.from(b64, 'base64');
      const as = `${x.name}.${f.replaceAll('/', '_')}`;
      fs.writeFileSync(path.join(OUT, as), buf);
      console.log(`roam-pair: ${as} ${buf.length} bytes`);
    } catch (e) { console.log(`roam-pair: no ${f} from ${x.name}: ${e}`); }
  }
  if (DOWNLOAD_LOG) {
    try {
      const [dl] = await Promise.all([x.page.waitForEvent('download', { timeout: 15000 }), x.page.click('#log-btn', { timeout: 15000 })]);
      await dl.saveAs(path.join(OUT, `${x.name}.download.log`));
      console.log(`roam-pair: ${x.name}.download.log`);
    } catch (e) { console.log(`roam-pair: no downloaded log from ${x.name}: ${e}`); }
  }
  x.log.end();
}
await browser.close();
if (crowd) crowd.kill('SIGTERM');
if (bounced) await bounced;
await stopServer();
serverLog.end();

// what the logs say
const summary = { mode: MODE, result: Object.fromEntries(inst.map((x, i) => [x.name, res[i]])) };
for (const x of inst) {
  // lines without the runner's time stamp and the thread tag
  const L = fs.readFileSync(path.join(OUT, `${x.name}.log`), 'utf8').split('\n').map(l => l.replace(/^[\d.]+ /, '').replace(/^\[\d+!?\] /, ''));
  const grab = (re) => L.filter(l => re.test(l));
  summary[x.name] = {
    vlan: grab(/virtual LAN (up|down|UDP)|\[net\]/).slice(0, 12),
    joins: grab(/\[roam\] JOIN|\[roam\] LEAVE/),
    waits: grab(/mp wait(peers|cars)/),
    slots: grab(/slot \d.*(ready|for )/).slice(0, 8),
    friends: grab(/\(\w+, [\w ]+, rtt [\d.]+ ms\)/).slice(-4),
    stats: grab(/virtual LAN (stats|closed)|datagrams sent|stats: \d friend/).slice(-6),
    blocked: grab(/BLOCKED/).length,
    parked: grab(/in the background: friends see you paused|\(paused|roam-pair\] page reports|app: (de)?activated/).slice(0, 8),
    exit: grab(/^EXIT /).pop() ?? null,
    net: grab(/^NET \{/).pop() ?? null,
  };
}
fs.writeFileSync(path.join(OUT, 'summary.json'), JSON.stringify(summary, null, 2));
console.log(JSON.stringify(summary, null, 2));
const pa = path.join(OUT, `${SPECS[0][0]}.pose.csv`);
for (const [name] of SPECS.slice(1)) {
  const pb = path.join(OUT, `${name}.pose.csv`);
  if (!fs.existsSync(pa) || !fs.existsSync(pb)) continue;
  console.log(`--- ${SPECS[0][0]} as shown by ${name}`);
  try { console.log(execFileSync('python3', [path.join(REPO, 'tools/roam_compare.py'), pa, pb], { encoding: 'utf8' })); }
  catch (e) { console.log(`roam_compare.py failed: ${e.stdout ?? ''}${e.stderr ?? e}`); }
}
