// Boot-movie test: runs the game from a fresh or
// seeded save with the movies on, timestamps every console line, optionally
// presses a key / clicks / presses a pad button every N ms until the title,
// and reports when each boot movie screen started and ended and how long after
// the last press it ended.
//
//   node web/tools/intro-test.mjs --name NAME [options]
//     --base URL        the local server (default http://localhost:8626)
//     --run DIR         out/web/runs/DIR (default intro)
//     --page P          hostcheck (default; no input layer), aicheck (input +
//                       audio attached) or play (WebGPU renderer)
//     --intro V         NFSU2_INTRO=V (skippable | original | off); unset: the default
//     --reg URL|none    registry seed (default none = first startup)
//     --save NAME       OPFS save directory (default intro-NAME); --keep: no &fresh
//     --script FILE     autotest script (default /tests/web/scripts/intro_idle.txt)
//     --drive MODE      none (default) | key:Enter | click | tap | pad:0 (standard-mapping
//                       index): trusted Playwright input every --every ms (default 500)
//                       until the top screen is --stop (default: the title)
//     --seed DIR        a save tree written into the save directory first (no &fresh);
//                       default $NFSU2_SAVE_SEED (Most Wanted's D3DX oracle module
//                       needs d3dx9_26.dll there and --env NFSMW_D3DX=/save/d3dx9_26.dll)
//     --env K=V         extra environment (repeatable)
//     --timeout S       time limit (default 240)
//     --qs K=V          extra page query parameter (repeatable)
//     --profile DIR     keep this Chrome profile (OPFS saves) between runs; with
//                       --keep and the same --save the next run is a later
//                       startup of that save (registry.txt, profiles)
//     --game ID         ug2 (default) or mw: the game's boot screens, default
//                       script and title (Most Wanted's server serves under /mw:
//                       --base http://localhost:PORT/mw)
//     --help            this text and the game's boot screens
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { GAMES, gameOf, usage, nothingFor, requireGame } from './rig-game.mjs';
import { seedSave } from './save-seed.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const argv = process.argv.slice(2);
// each game's boot: the default script, the boot movie screens up to the title in order, the screens that are
// boot movies (the driver starts at the first of them), the first boot movie's line, the title screen, and the
// [fng] line naming the screen on top (UG2: its top package; Most Wanted: the screen that came up last, as
// runtime/mw/fe_mw.c notes them, since its top package stays GarageMain.fng under the boot screens)
const INTRO_OF = {
  ug2: {
    script: '/tests/web/scripts/intro_idle.txt', title: 'UG_LS_Splash.fng',
    boot: ['MC_Bootup.fng', 'LS_BlankMovie.fng', 'LS_EAlogo.fng', 'LS_THXMovie.fng', 'LS_PSAMovie.fng', 'UG_LS_IntroFMV.fng', 'UG_LS_Splash.fng'],
    movie: /^LS_|^UG_LS_|^MC_Bootup/, first: /^\[fng\] f=\d+ (screen|top package) (LS_BlankMovie|LS_EAlogo)\.fng/,
    top: /^\[fng\] f=(\d+) top package (\S+)/,
  },
  mw: {
    script: '/tests/scripts/intro_idle.txt', title: 'MW_LS_Splash.fng',
    boot: ['MC_Bootup.fng', 'LS_EAlogo.fng', 'LS_PSA.fng', 'MW_LS_AttractFMV.fng', 'MW_LS_Splash.fng'],
    movie: /^LS_|^MW_LS_|^MC_Bootup/, first: /^\[fng\] f=\d+ screen LS_EAlogo\.fng/,
    top: /^\[fng\] f=(\d+) screen (\S+)/,
    under: /^GarageMain\.fng$/,          // comes up with the title, beneath it: not the screen on top
  },
};
const NONE = 'none';
const GAME = gameOf(argv), G = GAMES[GAME], I = INTRO_OF[GAME];
if (argv.includes('--help')) usage(import.meta.url, GAME, 'Boot screens', I ? [I.boot.join(' '),
  `title (the default --stop): ${I.title}`, `default --script: ${I.script}`] : [], NONE);
if (!I) nothingFor(GAME, NONE);
const o = { base: 'http://localhost:8626', run: 'intro', page: 'hostcheck', intro: '', reg: 'none', save: '', keep: false,
  script: I.script, drive: 'none', every: 500, stop: I.title, env: [], qs: [], timeout: 240, name: '',
  profile: '', seed: process.env.NFSU2_SAVE_SEED ?? '' };
for (let i = 0; i < argv.length; i++) {
  const a = argv[i], v = () => argv[++i];
  if (a === '--keep') o.keep = true;
  else if (a === '--game') v();                    // gameOf() above
  else if (a === '--env') o.env.push(v());
  else if (a === '--qs') o.qs.push(v());
  else if (a.startsWith('--') && a.slice(2) in o) o[a.slice(2)] = typeof o[a.slice(2)] === 'number' ? +v() : v();
  else { console.error(`unknown argument ${a}`); process.exit(2); }
}
if (!o.name) { console.error('--name is required'); process.exit(2); }
const OUT = path.join(ROOT, 'out/web/runs', o.run);
fs.mkdirSync(OUT, { recursive: true });

const env = { NFSU2_SAVEDIR: '/save', NFSU2_READONLY_DATA: '1', NFSU2_DISCORD: '0', NOFOCUS: '2', NFSU2_FNG_LOG: '1',
  NFSU2_STATUS_EVERY: '600', NOMOVIES: '0', SNDSYNC: '0', GPUVS: '1', FIXMAT: '0', FIXMAT_MODE: '3' };
if (o.intro) env.NFSU2_INTRO = o.intro;
for (const e of o.env) env[e.slice(0, e.indexOf('='))] = e.slice(e.indexOf('=') + 1);
const q = new URLSearchParams();
q.set('wasm', '/game.wasm'); q.set('pages', '16384'); q.append('arg', G.exe);
for (const [k, v] of Object.entries(env)) q.append('env', `${k}=${v}`);
q.set('save', o.save || `intro-${o.name}`); if (!o.keep && !o.seed) q.set('fresh', '');
if (o.reg !== 'none') q.set('reg', o.reg);
if (o.script) q.set('script', o.script);
for (const e of o.qs) q.append(e.slice(0, e.indexOf('=')), e.slice(e.indexOf('=') + 1));
const url = `${o.base}/test/${o.page}.html?${q}`;
await requireGame(o.base, GAME);

const usesPad = o.drive.startsWith('pad');
const launch = { ...pickBrowser(), headless: true, args: ['--enable-unsafe-webgpu'] };
const browser = o.profile ? null : await chromium.launch(launch);
const view = { viewport: { width: 1280, height: 720 }, ...o.drive === 'tap' ? { hasTouch: true } : {} };
const ctx = o.profile ? await chromium.launchPersistentContext(o.profile, { ...launch, ...view })
  : await browser.newContext(view);
const page = ctx.pages()[0] ?? await ctx.newPage();
if (usesPad) await page.addInitScript(() => {
  const pad = { id: 'Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 0b13)', index: 0, connected: true,
    mapping: 'standard', timestamp: 0, axes: [0, 0, 0, 0], vibrationActuator: null,
    buttons: Array.from({ length: 17 }, () => ({ pressed: false, touched: false, value: 0 })) };
  window.__setPad = (i, v) => { pad.buttons[i] = { pressed: v > 0.5, touched: v > 0, value: v }; pad.timestamp = performance.now(); };
  navigator.getGamepads = () => [pad, null, null, null];
});

const lines = [];
let t0 = 0, top = '', exit = null;
const now = () => Date.now() - t0;
const add = (t) => { const ms = now(); lines.push({ ms, t }); };
page.on('console', m => {
  let t = m.text();
  const g = /^\[\d+!?\] ([^]*)$/.exec(t); if (g) t = g[1];
  const e = /^EXIT (-?\d+)$/.exec(t); if (e) exit = +e[1];
  const tp = I.top.exec(t); if (tp && !I.under?.test(tp[2])) top = tp[2];
  add(t);
});
page.on('pageerror', e => add(`PAGEERROR ${e}`));
page.on('crash', () => add('CRASH (renderer process)'));
if (o.seed) console.log(`save seed: ${await seedSave(page, o.base, q.get('save'), o.seed)} files from ${o.seed}`);
t0 = Date.now();
await page.goto(url);
const done = page.waitForFunction(() => document.title === 'DONE', null, { timeout: o.timeout * 1000, polling: 250 }).then(() => true, () => false);

// the driver: one press every o.every ms from the first boot movie until the title
let driving = o.drive !== 'none';
const sleep = ms => new Promise(r => setTimeout(r, ms));
const driver = (async () => {
  if (!driving) return;
  while (!I.movie.test(top)) { await sleep(20); if (exit !== null) return; }
  let n = 0;
  while (driving && exit === null) {
    if (top === o.stop) break;
    n++;
    add(`[drive] press ${o.drive} #${n} (top ${top})`);
    if (o.drive.startsWith('key:')) await page.keyboard.press(o.drive.slice(4), { delay: 60 });
    else if (o.drive === 'click') await page.mouse.click(640, 360, { delay: 60 });
    else if (o.drive === 'tap') await page.touchscreen.tap(640, 360);
    else if (usesPad) { const b = +(o.drive.split(':')[1] ?? 0);
      await page.evaluate(b => window.__setPad(b, 1), b); await sleep(80); await page.evaluate(b => window.__setPad(b, 0), b); }
    await sleep(o.every);
  }
  add(`[drive] stopped after ${n} presses (top ${top})`);
})();
const ok = await done;
driving = false; await driver;
if (!ok) add('TIMEOUT');
await ctx.close(); await browser?.close();

// ---- analysis ----
const fmt = ms => (ms / 1000).toFixed(2).padStart(6) + ' s';
const BOOT = I.boot;
const tops = lines.map(l => ({ l, m: I.top.exec(l.t) })).filter(x => x.m && !I.under?.test(x.m[2]))
  .map(({ l, m }) => ({ ms: l.ms, f: +m[1], name: m[2] }));
const presses = lines.filter(l => /^\[drive\] press|^\[autotest\] .*(tapuntil|tappaduntil) tap|^\[autotest\] .*LOG press/.test(l.t)).map(l => l.ms);
const skips = lines.filter(l => /^\[intro\]/.test(l.t));
const screens = [];
for (let i = 0; i < tops.length; i++) {
  if (!BOOT.includes(tops[i].name)) continue;
  const end = tops[i + 1];
  const s = { name: tops[i].name, start: tops[i].ms, f0: tops[i].f, end: end?.ms ?? null, f1: end?.f ?? null, next: end?.name ?? null };
  if (s.end !== null) {
    const before = presses.filter(p => p >= s.start && p <= s.end);
    s.presses = before.length;
    s.afterPress = before.length ? s.end - before[before.length - 1] : null;
  }
  screens.push(s);
}
const firstLine = lines.find(l => I.first.test(l.t));
const first = firstLine ? { ms: firstLine.ms, f: +/f=(\d+)/.exec(firstLine.t)[1] } : null;
const title = tops.find(t => t.name === I.title);
const res = { name: o.name, url, page: o.page, intro: o.intro || '(default)', reg: o.reg, drive: o.drive, every: o.every, exit, ok,
  bootToTitle: first && title ? title.ms - first.ms : null, framesToTitle: first && title ? title.f - first.f : null,
  screens, presses: presses.length, sequence: tops.map(t => t.name),
  // each [intro] line; a skip with the time since the press before it (the decision latency)
  skips: skips.map(l => { const p = presses.filter(x => x <= l.ms).pop();
    return `${fmt(l.ms)} ${l.t}${/ skipped by /.test(l.t) && p !== undefined ? `  [${l.ms - p} ms after the press]` : ''}`; }) };
fs.writeFileSync(path.join(OUT, `${o.name}.log`), lines.map(l => `${String(l.ms).padStart(7)} ${l.t}`).join('\n') + '\n');
fs.writeFileSync(path.join(OUT, `${o.name}.json`), JSON.stringify(res, null, 1));
console.log(`${o.name}: page ${o.page}, intro ${res.intro}, reg ${o.reg}, drive ${o.drive}${o.drive !== 'none' ? ` every ${o.every} ms` : ''}, exit ${exit}${ok ? '' : ' (TIMEOUT)'}`);
for (const s of screens)
  console.log(`  ${s.name.padEnd(20)} ${fmt(s.start)} -> ${s.end === null ? '   (open)' : fmt(s.end)}  ${s.end === null ? '' : fmt(s.end - s.start)}  frames ${s.f1 === null ? '-' : s.f1 - s.f0}` +
    (s.presses ? `  presses ${s.presses}, ended ${s.afterPress} ms after the last` : ''));
console.log(`  first boot movie -> title: ${res.bootToTitle === null ? 'not reached' : fmt(res.bootToTitle) + ` (${res.framesToTitle} frames)`}`);
for (const k of res.skips) console.log(`  ${k}`);
console.log(`  log: ${path.relative(ROOT, path.join(OUT, `${o.name}.log`))}`);
process.exitCode = ok && exit === 0 ? 0 : 1;
