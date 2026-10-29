// "Preload the game data" (Game data on this PC) in headless Chrome,
// against a server of its own over a made-up game folder, in sizes that hit
// the edges of the 1 MiB blocks and the 8-block range requests: random bytes (they
// go as they are) and bytes that compress (they go compressed):
//   node web/tools/predownload-test.mjs [--port 8657] [--out <dir>] [--headed]
// Steps (each prints PASS/FAIL with evidence):
//   fresh     the page offers the folder but the movies (the game's files only) and says nothing is here yet
//   full      the download ends with every file but the movies in the cache, byte for byte, and the page says
//             so; it came in compressed blocks, zstd (or br, as the server keeps them) where the bytes compress,
//             as they are where not
//   resume    with slow answers: Stop keeps what came, a reload offers the rest, and the rest completes
//   retry     busy answers (503) and bodies cut short are tried again, and the result is still exact
//   fail      a server that refuses one file (404): the download stops and says why, keeping the files before it
//   play      Play pauses a running download (its worker is gone while the game runs), the game's end resumes it
//   tabs      a second tab on the same saves cannot start one
//   delete    "Delete the game data on this PC" while it preloads stops it and leaves nothing
//   language  with French chosen in Settings: the French language pack's files but its movies, the folder's
//             but the file the pack has in its place, and no other language's
//             (no step before fetches a texture pack: Menus and HUD is on Original by default)
//   texpack   with Menus and HUD on Upscaled: the game data counted gains TEXPACKS/uhud.pak, the rest to preload
//             is the pack, and it is all that is fetched
// The game module is out/web/nfsu2.wasm, as for the server (--wasm to use another).
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { serverCommand } from './server-cmd.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const PORT = +arg('--port', 8657), BASE = `http://localhost:${PORT}`;
const OUT = path.resolve(arg('--out', path.join(ROOT, 'out/web/predownload-test')));
const WASM = path.resolve(arg('--wasm', path.join(ROOT, 'out/web/nfsu2.wasm')));
const M = 1 << 20;

// ---- the made-up game folder ----
const DATA = path.join(OUT, 'data');
fs.rmSync(OUT, { recursive: true, force: true });
const files = new Map();                  // path -> sha256 of what the server holds
let seed = 1;
/** random bytes, or (packs) bytes of 2 bits each that compress to about a quarter */
function make(rel, size, packs = false) {
  const p = path.join(DATA, rel);
  fs.mkdirSync(path.dirname(p), { recursive: true });
  // AES-128-CTR over zeros, keyed by a counter: random-looking, the same every run
  const key = crypto.createHash('sha256').update(String(seed++)).digest().subarray(0, 16);
  const buf = crypto.createCipheriv('aes-128-ctr', key, Buffer.alloc(16)).update(Buffer.alloc(size));
  if (packs) for (let i = 0; i < size; i++) buf[i] = (buf[i] & 3) * 0x41;
  fs.writeFileSync(p, buf);
  files.set(rel, crypto.createHash('sha256').update(buf).digest('hex'));
}
make('SPEED2.EXE', 3 * M + 17);
make('FRONTEND/EMPTY.BIN', 0);
make('GLOBAL/ONEBLOCK.BIN', M, true);
make('TRACKS/RUNPLUSONE.BIN', 8 * M + 1, true);
make('MOVIES/BIG.VP6', 40 * M + 123);
make('SOUND/TINY.BIN', 100);
make('SOUND/SFX/Name With Space & #.bin', 2 * M - 1);
for (let i = 0; i < 24; i++) make(`CARS/CAR${String(i).padStart(2, '0')}/GEOMETRY.BIN`, 1000 + (i * 397_000) % (3 * M), i % 2 === 0);
// language packs (runtime/language.c): a file there is read in place of the folder's file of the same path
make('LANGPACKS/french/SOUND/TINY.BIN', 300);
make('LANGPACKS/french/speech/UG2_Speech_FR.big', 3 * M + 5, true);
make('LANGPACKS/french/MOVIES/BIG.VP6', 5 * M);
make('LANGPACKS/german/speech/UG2_Speech_GR.big', 2 * M);
// texture packs (runtime/web/texrep_web.c): only the one picked is fetched (none by default)
make('TEXPACKS/uhud.pak', 2 * M + 3, true);
make('TEXPACKS/other.pak', M);
fs.writeFileSync(path.join(DATA, 'NOTES.TXT'), 'not the game: never served');
// what the download fetches for a game in this language: all but the movies, of the language packs only its own,
// and not the folder's files its pack has in their place; of the texture packs only the one picked (texpack)
function aheadOf(lang, texpack = '') {
  const own = [...files.keys()].filter(f => f.startsWith(`LANGPACKS/${lang}/`));
  const covered = new Set(own.map(f => f.split('/').slice(2).join('/')));
  return [...files.keys()].filter(f => !f.startsWith('LANGPACKS/') && !covered.has(f)).concat(own)
    .filter(f => f.split('/')[f.startsWith('LANGPACKS/') ? 2 : 0] !== 'MOVIES')
    .filter(f => !f.startsWith('TEXPACKS/') || f === `TEXPACKS/${texpack}.pak`);
}
const AHEAD = aheadOf('english');
const sizeOf = list => list.reduce((a, f) => a + fs.statSync(path.join(DATA, f)).size, 0);
const TOTAL = sizeOf(AHEAD);

// ---- the server ----
const server = spawn(...serverCommand(['--data', DATA, '--http-port', String(PORT), '--dev', '--state', path.join(OUT, 'state'), '--wasm', WASM]),
  { stdio: ['ignore', 'pipe', 'pipe'] });
let serverLog = '';
server.stdout.on('data', d => { serverLog += d; }); server.stderr.on('data', d => { serverLog += d; });
const stopServer = () => { try { server.kill(); } catch { /* gone */ } };
process.on('exit', stopServer);
for (let i = 0; ; i++) {
  try { if ((await fetch(`${BASE}/data-manifest`)).ok) break; } catch { /* not yet */ }
  if (i > 100 || server.exitCode !== null) { console.error(`the server did not start:\n${serverLog}`); process.exit(2); }
  await new Promise(r => setTimeout(r, 100));
}

const results = [];
function report(step, ok, evidence) {
  results.push({ step, ok });
  console.log(`${ok ? 'PASS' : 'FAIL'} ${step}: ${typeof evidence === 'string' ? evidence : JSON.stringify(evidence)}`);
}

const pc = await chromium.launchPersistentContext(path.join(OUT, 'profile'), { ...pickBrowser(), headless: !process.argv.includes('--headed'),
  viewport: { width: 1280, height: 900 }, args: ['--enable-unsafe-webgpu'] });
// requests for the game's files, whoever makes them (the page's workers too)
let dataRequests = 0;
pc.on('request', r => { if (/^\/data(-blocks)?\//.test(new URL(r.url()).pathname)) dataRequests++; });
// the runs of blocks the page asked for, with the encoding they came in (Playwright reports a worker's
// responses, not their sizes: onWire() asks for the same runs again, as Chrome does, and counts the bytes)
const wire = [];
pc.on('response', r => {
  const u = new URL(r.url());
  if (u.pathname.startsWith('/data-blocks/') && r.status() === 200)
    wire.push({ url: u.pathname + u.search, path: decodeURIComponent(u.pathname.replace(/^\/data-blocks\/(v2\/)?/, '')), b: +u.searchParams.get('b'), n: +u.searchParams.get('n'),
      enc: r.headers()['content-encoding'] ?? 'none' });
});
const onWire = async () => {
  let n = 0;
  for (const w of wire) {
    n += await new Promise((res, rej) => http.get({ host: '127.0.0.1', port: PORT, path: w.url, headers: { host: `localhost:${PORT}`, 'accept-encoding': 'gzip, deflate, br, zstd' } },
      r => { let k = 0; r.on('data', d => { k += d.length; }); r.on('end', () => res(k)); }).on('error', rej));
  }
  return n;
};
const ranges = [];
pc.on('request', r => { if (new URL(r.url()).pathname.startsWith('/data/') && r.headers().range) ranges.push(r.url()); });
const page = pc.pages()[0] ?? await pc.newPage();
page.on('pageerror', e => console.log(`  PAGEERROR ${e}`));
const status = () => page.evaluate(() => window.__nfsu2.download());
const text = sel => page.locator(sel).textContent();
async function open() {
  await page.goto(`${BASE}/`);
  await page.waitForFunction(() => ['ready', 'error'].includes(window.__nfsu2?.phase), null, { timeout: 60000 });
  await page.waitForFunction(() => !/Counting/.test(document.querySelector('#cache-info').textContent), null, { timeout: 30000 });
}
async function until(f, ms = 60000) {
  const t0 = Date.now();
  for (;;) { const s = await status(); if (f(s)) return s; if (Date.now() - t0 > ms) return s; await page.waitForTimeout(50); }
}
/** every file the download fetches (aheadOf), as the cache holds it: all blocks marked and the bytes the server
 *  has; and nothing else, no movie */
async function cacheMatches(ahead = AHEAD) {
  const got = await page.evaluate(async () => {
    const m = await (await fetch('/data-manifest')).json();
    const d = await (await navigator.storage.getDirectory()).getDirectoryHandle('cache').catch(() => null);
    const out = {};
    for (const f of m.files.filter(f => !f.d)) {
      const k = `${f.p.replace(/[^A-Za-z0-9._-]/g, '_')}.${f.s}.${f.m}`;
      try {
        const have = new Uint8Array(await (await (await d.getFileHandle(k + '.have')).getFile()).arrayBuffer());
        const data = new Uint8Array(await (await (await d.getFileHandle(k)).getFile()).arrayBuffer()).subarray(0, f.s);
        const hex = [...new Uint8Array(await crypto.subtle.digest('SHA-256', data))].map(b => b.toString(16).padStart(2, '0')).join('');
        out[f.p] = { full: have.length === Math.ceil(f.s / (1 << 20)) && have.every(b => b === 1), size: data.length, hex };
      } catch { out[f.p] = null; }
    }
    return out;
  });
  // an empty file needs nothing in the cache (a read of it never asks)
  const bad = ahead.filter(p => got[p] ? !got[p].full || got[p].hex !== files.get(p) : fs.statSync(path.join(DATA, p)).size > 0);
  const extra = Object.keys(got).filter(p => got[p] && !ahead.includes(p));
  return { ok: !bad.length && !extra.length && Object.keys(got).length === files.size, bad, extra };
}
async function clearData() {
  await page.click('#clear-cache');
  await page.click('#confirm-ok');
  await page.waitForFunction(() => /deleted/.test(document.querySelector('#data-msg').textContent), null, { timeout: 15000 });
}
const slow = ms => pc.route('**/data-blocks/**', async r => { await new Promise(res => setTimeout(res, ms)); await r.continue().catch(() => { /* unrouted meanwhile */ }); });
const unroute = () => pc.unrouteAll({ behavior: 'ignoreErrors' });

try {
  // ------------------------------------------------------------ fresh --
  await open();
  {
    const btn = await text('#fetch-all'), info = await text('#cache-info');
    const want = `Preload the game data (${(TOTAL / 1e6).toFixed(1)} MB)`;
    report('fresh', btn === want && /^Nothing here yet/.test(info) && await page.locator('#data-bar').isHidden(), { btn, want, info });
  }

  // ------------------------------------------------------------- full --
  {
    const before = dataRequests;
    wire.length = 0;
    await page.click('#fetch-all');
    const s = await until(s => s.state === 'done' || s.state === 'error');
    await page.waitForFunction(() => /^The game data \(/.test(document.querySelector('#cache-info').textContent), null, { timeout: 15000 }).catch(() => {});
    await page.waitForTimeout(300);                                   // the last requestfinished events
    const c = await cacheMatches();
    const info = await text('#cache-info'), msg = await text('#data-msg');
    // each run went compressed exactly when its bytes compress (made with packs, and more than a few of them: the
    // last byte of TRACKS/RUNPLUSONE.BIN does not), and the whole took fewer bytes
    const packed = p => /^(GLOBAL|TRACKS)\//.test(p) || /^CARS\/CAR(\d\d)\//.test(p) && +/CAR(\d\d)/.exec(p)[1] % 2 === 0;
    const runBytes = w => Math.min(fs.statSync(path.join(DATA, w.path)).size, (w.b + w.n) * M) - w.b * M;
    const wrongEnc = wire.filter(w => (w.enc === 'zstd' || w.enc === 'br') !== (packed(w.path) && runBytes(w) > 16)).map(w => `${w.path}#${w.b}:${w.enc}`);
    const bytesOnWire = await onWire();
    report('full', s.state === 'done' && s.have === TOTAL && s.total === TOTAL && c.ok && await page.locator('#fetch-all').isHidden() && /^The game data \(/.test(info) &&
      !ranges.length && wire.length > 0 && !wrongEnc.length && bytesOnWire < TOTAL * 0.8,
      { state: s.state, have: s.have, total: TOTAL, requests: dataRequests - before, runs: wire.length, rangeRequests: ranges.length, wrongEncoding: wrongEnc,
        onWireMB: +(bytesOnWire / 1e6).toFixed(1), ofMB: +(TOTAL / 1e6).toFixed(1), mismatched: c.bad, extra: c.extra, info, msg });
  }

  // ----------------------------------------------------------- resume --
  {
    await clearData();
    await slow(400);
    await page.click('#fetch-all');
    const part = await until(s => s.have > 4 * M);
    await page.click('#fetch-all');                                    // Stop preloading
    const stopped = await until(s => s.state === 'idle', 5000);
    const label = await page.waitForFunction(() => /^Preload the rest/.test(document.querySelector('#fetch-all').textContent) && document.querySelector('#fetch-all').textContent, null, { timeout: 15000 })
      .then(h => h.jsonValue()).catch(() => text('#fetch-all'));
    await unroute();
    await open();
    const after = await text('#fetch-all');
    const before = dataRequests;
    await page.click('#fetch-all');
    const s = await until(s => s.state === 'done' || s.state === 'error');
    const c = await cacheMatches();
    report('resume', part.have > 0 && stopped.state === 'idle' && stopped.have < TOTAL && /^Preload the rest/.test(label) && after === label && s.state === 'done' && c.ok,
      { stoppedAt: stopped.have, total: TOTAL, label, afterReload: after, state: s.state, restRequests: dataRequests - before, mismatched: c.bad });
  }

  // ------------------------------------------------------------ retry --
  {
    await clearData();
    let busy = 0, cut = 0;
    await pc.route('**/data-blocks/**', async r => {
      if (busy < 3) { busy++; return r.fulfill({ status: 503, body: 'busy' }); }
      if (cut < 3) {
        cut++;                                       // taken before the await: handlers run side by side
        // the bytes themselves (identity), cut in half: what a dropped connection leaves
        const res = await r.fetch({ headers: { ...r.request().headers(), 'accept-encoding': 'identity' } });
        const body = await res.body();
        if (res.status() === 200 && body.length > 1000) return r.fulfill({ status: 200, contentType: 'application/octet-stream', body: body.subarray(0, body.length >> 1) });
        cut--;
        return r.fulfill({ response: res, body });
      }
      return r.continue();
    });
    await page.click('#fetch-all');
    const s = await until(s => s.state === 'done' || s.state === 'error', 120000);
    await unroute();
    const c = await cacheMatches();
    report('retry', busy === 3 && cut === 3 && s.state === 'done' && c.ok, { busy, cut, state: s.state, error: s.error, mismatched: c.bad });
  }

  // ------------------------------------------------------------- fail --
  {
    await clearData();
    await pc.route('**/data-blocks/v2/SOUND/**', r => r.fulfill({ status: 404, body: 'no' }));
    await page.click('#fetch-all');
    const s = await until(s => s.state === 'done' || s.state === 'error');
    await unroute();
    await page.waitForFunction(() => /^Preload the rest/.test(document.querySelector('#fetch-all').textContent), null, { timeout: 15000 }).catch(() => {});
    const msg = await text('#data-msg'), btn = await text('#fetch-all');
    report('fail', s.state === 'error' && /HTTP 404/.test(s.error) && /^Preloading stopped: .*HTTP 404.*What it preloaded is kept\.$/.test(msg) && /^Preload the rest/.test(btn),
      { state: s.state, error: s.error, msg, btn });
  }

  // ------------------------------------------------------------- play --
  {
    await clearData();
    await slow(300);
    await page.click('#fetch-all');
    await until(s => s.have > 2 * M);
    const workers = () => page.workers().map(w => new URL(w.url()).pathname.replace(/^\/host\/|\.js$/g, '')).filter(n => n !== 'game-worker');
    const before = workers();
    // free roam answered, so the first Play does not ask (shell.ts askRoam)
    await page.evaluate(async () => {
      const fh = await (await (await navigator.storage.getDirectory()).getDirectoryHandle('save', { create: true })).getFileHandle('settings.ini', { create: true });
      const text = await (await fh.getFile()).text();
      const w = await fh.createWritable(); await w.write(`${text}\nroam = off\n`); await w.close();
    });
    await page.click('#play');
    const paused = await until(s => s.state === 'paused', 10000);
    // the game made from random bytes does not get far: its I/O worker is up, then the game ends or is stopped
    await page.waitForFunction(() => window.__nfsu2.session || window.__nfsu2.phase === 'exited', null, { timeout: 30000 });
    const during = workers();
    await page.evaluate(() => window.__nfsu2.session?.terminate());
    await page.waitForFunction(() => window.__nfsu2.phase === 'exited', null, { timeout: 30000 });
    const resumed = await until(s => s.state !== 'paused', 10000);
    // Playwright hears of a new worker a moment after it starts
    for (let i = 0; i < 40 && !workers().includes('predownload-worker'); i++) await page.waitForTimeout(50);
    const after = workers();
    await unroute();
    const s = await until(s => s.state === 'done' || s.state === 'error');
    report('play', before.includes('predownload-worker') && paused.state === 'paused' && !during.includes('predownload-worker') &&
      resumed.state === 'running' && after.includes('predownload-worker') && s.state === 'done' && (await cacheMatches()).ok,
      { workersBefore: before, paused: paused.state, workersDuringGame: during, afterGame: resumed.state, workersAfter: after, end: s.state });
  }

  // ------------------------------------------------------------- tabs --
  {
    await clearData();
    const p2 = await pc.newPage();
    await p2.goto(`${BASE}/`);
    await p2.waitForFunction(() => window.__nfsu2?.phase === 'waiting' && !/Counting/.test(document.querySelector('#cache-info').textContent), null, { timeout: 30000 });
    const disabled = await p2.locator('#fetch-all').isDisabled();
    await p2.locator('#fetch-all').click({ force: true }).catch(() => {});
    await p2.waitForTimeout(500);
    const st = await p2.evaluate(() => window.__nfsu2.download());
    await p2.close();
    report('tabs', disabled && st.state === 'idle', { disabled, state: st.state });
  }

  // ----------------------------------------------------------- delete --
  {
    await slow(300);
    await page.click('#fetch-all');
    await until(s => s.have > 2 * M);
    await page.click('#clear-cache');
    const ask = await text('#confirm-body');
    await page.click('#confirm-ok');
    await page.waitForFunction(() => /deleted/.test(document.querySelector('#data-msg').textContent), null, { timeout: 15000 });
    await unroute();
    const st = await status();
    const left = await page.evaluate(async () => { try { await (await navigator.storage.getDirectory()).getDirectoryHandle('cache'); return true; } catch { return false; } });
    const info = await text('#cache-info');
    report('delete', /stops preloading/.test(ask) && st.state === 'idle' && !left && /^Nothing here yet/.test(info), { ask, state: st.state, cacheLeft: left, info });
  }

  // --------------------------------------------------------- language --
  {
    const fr = aheadOf('french'), total = sizeOf(fr), want = `Preload the game data (${(total / 1e6).toFixed(1)} MB)`;
    await page.$eval('#set-language', el => { el.value = 'french'; el.dispatchEvent(new Event('change')); });
    const btn = await page.waitForFunction(w => document.querySelector('#fetch-all').textContent === w && w, want, { timeout: 15000 })
      .then(h => h.jsonValue()).catch(() => text('#fetch-all'));
    await page.click('#fetch-all');
    const s = await until(s => s.state === 'done' || s.state === 'error');
    await page.waitForTimeout(300);
    const c = await cacheMatches(fr);
    report('language', btn === want && s.state === 'done' && s.have === total && s.total === total && c.ok &&
      fr.includes('LANGPACKS/french/SOUND/TINY.BIN') && !fr.includes('SOUND/TINY.BIN') && !fr.some(f => /german|MOVIES/.test(f)),
      { btn, want, state: s.state, have: s.have, total, mismatched: c.bad, extra: c.extra });
  }

  // ---------------------------------------------------------- texpack --
  {
    // everything French is here (the language step): with the pack the count gains it, the rest is the pack, and it
    // is all that comes
    const up = aheadOf('french', 'uhud'), total = sizeOf(up), want = `Preload the rest (${(sizeOf(['TEXPACKS/uhud.pak']) / 1e6).toFixed(1)} MB)`;
    wire.length = 0;
    await page.$eval('input[name="set-ui_textures"][value="uhud"]', el => { el.checked = true; el.dispatchEvent(new Event('change')); });
    const btn = await page.waitForFunction(w => document.querySelector('#fetch-all').textContent === w && w, want, { timeout: 15000 })
      .then(h => h.jsonValue()).catch(() => text('#fetch-all'));
    await page.click('#fetch-all');
    const s = await until(s => s.state === 'done' || s.state === 'error');
    await page.waitForTimeout(300);
    const c = await cacheMatches(up), fetched = [...new Set(wire.map(w => w.path))];
    report('texpack', btn === want && s.state === 'done' && s.have === total && s.total === total && c.ok &&
      up.includes('TEXPACKS/uhud.pak') && !up.includes('TEXPACKS/other.pak') && !aheadOf('french').some(f => f.startsWith('TEXPACKS/')) &&
      fetched.join() === 'TEXPACKS/uhud.pak',
      { btn, want, state: s.state, have: s.have, total, fetched, mismatched: c.bad, extra: c.extra });
  }
} catch (e) {
  report('run', false, String(e?.stack ?? e));
} finally {
  await pc.close();
  stopServer();
}
const failed = results.filter(r => !r.ok);
console.log(`${results.length - failed.length}/${results.length} passed${failed.length ? `; failed: ${failed.map(r => r.step).join(', ')}` : ''}`);
if (!failed.length) fs.rmSync(OUT, { recursive: true, force: true });
process.exit(failed.length ? 1 : 0);
