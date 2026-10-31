// Offline mode in headless Chrome, with the real game folder and a server this tool starts and stops:
//   node web/tools/offline-test.mjs [--data "<game folder>"] [--port 8659] [--out <dir>] [--headed]
// Steps (each prints PASS/FAIL with evidence):
//   kept      online: the service worker controls the page and keeps the page, its scripts, the module and the
//             list of game files; "Preload the game data" fills the cache
//   offline   the server gone: the page reloads from the copies, says it is offline, and Play gets to the title
//             screen with the movies off and no game data asked of the network
//   proxy502  a proxy in front of a stopped server (every answer 502): the same, from the copies
//   silent    a server that takes the connection and never answers: the page after NAV_WAIT_MS, from the copies
//   back      the server again: the page is online, from the server
//   deploy    the server restarts with a new version while the page reloads: the page comes from the copies (the
//             old version), and once the server answers it reloads by itself into the new one, inside the service
//             worker's offline window
// On Windows (the server in WSL) the steps stop at proxy502: WSL's mirrored networking holds the port on Windows'
// side for 25 s and more after the server ended, so nothing there can stand on it (2026-10-21).
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { serverCommand } from './server-cmd.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const DATA = path.resolve(arg('--data', process.env.NFSU2_DATA || path.join(os.homedir(), 'projects/nsfu2/Need for Speed - Underground 2')));
const PORT = +arg('--port', 8659), BASE = `http://localhost:${PORT}`;
const OUT = path.resolve(arg('--out', path.join(ROOT, 'out/web/offline-test')));
const WASM = path.resolve(arg('--wasm', path.join(ROOT, 'out/web/nfsu2.wasm')));
if (!fs.existsSync(path.join(DATA, 'SPEED2.EXE'))) { console.error(`no game folder at ${DATA} (--data)`); process.exit(2); }
fs.rmSync(OUT, { recursive: true, force: true });
fs.mkdirSync(OUT, { recursive: true });

let server = null;
async function startServer(wasm = WASM) {
  server = spawn(...serverCommand(['--data', DATA, '--http-port', String(PORT), '--dev', '--state', path.join(OUT, 'state'), '--wasm', wasm]),
    { stdio: ['ignore', 'pipe', 'pipe'] });
  await new Promise((res, rej) => {
    let out = '';
    const t = setTimeout(() => rej(new Error('server did not start: ' + out)), 20000);
    server.stdout.on('data', d => { out += d; if (out.includes('  access    :')) { clearTimeout(t); res(); } });
    server.on('exit', c => { clearTimeout(t); rej(new Error('server exited ' + c + ' ' + out)); });
  });
}
async function stopServer() {
  if (!server || server.exitCode !== null) return;
  const gone = new Promise(r => server.once('exit', r)); server.kill(); await gone;
}
process.on('exit', () => { try { server?.kill(); } catch { /* gone */ } });
/** something else on the port: a proxy answering 502, or a peer that never answers */
function stand(kind) {
  const srv = kind === '502' ? http.createServer((q, s) => { s.writeHead(502, { 'content-type': 'text/plain' }); s.end('Bad gateway'); })
    : net.createServer(sock => { sock.on('error', () => {}); /* never answers */ });
  return new Promise(res => srv.listen(PORT, '127.0.0.1', () => res(srv)));
}
const closeStand = srv => new Promise(res => { srv.closeAllConnections?.(); srv.close(() => res()); for (const s of srv.__socks ?? []) s.destroy(); });

const results = [];
function report(step, ok, evidence) {
  results.push({ step, ok });
  console.log(`${ok ? 'PASS' : 'FAIL'} ${step}: ${typeof evidence === 'string' ? evidence : JSON.stringify(evidence)}`);
}

await startServer();
const pc = await chromium.launchPersistentContext(path.join(OUT, 'profile'), { ...pickBrowser(), headless: !process.argv.includes('--headed'),
  viewport: { width: 1280, height: 800 }, args: ['--enable-unsafe-webgpu'] });
// game data the page asked of the network and did not get
const failedData = [];
pc.on('requestfailed', r => { if (/^\/data(-blocks)?\//.test(new URL(r.url()).pathname)) failedData.push(r.url()); });
const page = pc.pages()[0] ?? await pc.newPage();
page.on('pageerror', e => console.log(`  PAGEERROR ${e}`));
// no page address parameters: offline the page cannot ask whether the server is a development server, so it
// takes none (shell.ts); the game's screen log (for the title screen) is in settings.ini instead, with free
// roam answered (a player's first Play asks)
async function open(ms = 90000) {
  const t0 = Date.now();
  await page.goto(`${BASE}/`, { timeout: ms });
  await page.waitForFunction(() => ['ready', 'error'].includes(window.__nfsu2?.phase), null, { timeout: ms });
  return Date.now() - t0;
}
/** Play to the title screen: ms, whether it got there, the log lines about offline */
async function toTitle() {
  await page.evaluate(() => window.__nfsu2.log.clear());
  const f0 = failedData.length, t0 = Date.now();
  await page.click('#play');
  const ok = await page.waitForFunction(() => window.__nfsu2.log.tail(600).some(l => /top package UG_LS_Splash\.fng/.test(l)), null, { timeout: 180000, polling: 200 })
    .then(() => true, () => false);
  const ms = Date.now() - t0;
  const lines = (await page.evaluate(() => window.__nfsu2.log.tail(2000))).filter(l => /\[app\] offline|NOMOVIES/.test(l));
  await page.evaluate(() => window.__nfsu2.session?.terminate());
  await page.waitForFunction(() => window.__nfsu2.phase === 'exited', null, { timeout: 30000 }).catch(() => {});
  return { ok, ms, lines, failedData: failedData.slice(f0).length };
}
const state = () => page.evaluate(() => ({ phase: window.__nfsu2.phase, offline: window.__nfsu2.offline(), sub: document.querySelector('#status-sub').textContent,
  controlled: !!navigator.serviceWorker.controller }));
const loadedRev = () => page.evaluate(() => window.__nfsu2.checks().find(c => c.id === 'module')?.label.split(' ').pop());
/** a deploy's module: this one under another rev (the page tells versions by rev) */
function deployed() {
  const dir = path.join(OUT, 'deployed'), info = JSON.parse(fs.readFileSync(WASM.replace(/\.wasm$/, '.json'), 'utf8'));
  fs.mkdirSync(dir, { recursive: true });
  fs.copyFileSync(WASM, path.join(dir, 'nfsu2.wasm'));
  fs.writeFileSync(path.join(dir, 'nfsu2.json'), JSON.stringify({ ...info, rev: `${info.rev}-deployed` }));
  return { wasm: path.join(dir, 'nfsu2.wasm'), rev: `${info.rev}-deployed` };
}

try {
  // ------------------------------------------------------------- kept --
  {
    await open();
    await page.waitForFunction(async () => {
      try {
        const fh = await (await (await navigator.storage.getDirectory()).getDirectoryHandle('save')).getFileHandle('settings.ini');
        const text = await (await fh.getFile()).text();
        if (!/^NFSU2_FNG_LOG = 1$/m.test(text)) { const w = await fh.createWritable(); await w.write(text + '\nNFSU2_FNG_LOG = 1\nroam = off\n'); await w.close(); }
        return true;
      } catch { return false; }
    }, null, { timeout: 30000, polling: 250 });
    await page.waitForFunction(() => !!navigator.serviceWorker.controller, null, { timeout: 30000 });
    const kept = await page.waitForFunction(async () => {
      const c = await caches.open('nfsu2-offline-2'), keys = (await c.keys()).map(r => new URL(r.url).pathname);
      return keys.length >= 10 && keys;
    }, null, { timeout: 60000, polling: 500 }).then(h => h.jsonValue(), () => null);
    await page.waitForFunction(() => !/Counting/.test(document.querySelector('#cache-info').textContent), null, { timeout: 30000 });
    await page.click('#fetch-all');
    await page.waitForFunction(() => ['done', 'error'].includes(window.__nfsu2.download().state), null, { timeout: 600000, polling: 500 });
    const dl = await page.evaluate(() => window.__nfsu2.download());
    report('kept', !!kept && dl.state === 'done', { kept: kept?.sort(), download: dl.state, gameData: dl.total });
  }

  // ---------------------------------------------------------- offline --
  {
    await stopServer();
    const ms = await open();
    const st = await state();
    const play = await toTitle();
    report('offline', st.phase === 'ready' && st.offline && st.controlled && /^Offline/.test(st.sub) && play.ok && play.failedData === 0 &&
      play.lines.some(l => /movies are off/.test(l)), { pageToReadyMs: ms, ...st, playToTitleMs: play.ms, title: play.ok, failedData: play.failedData, log: play.lines });
  }

  // --------------------------------------------------------- proxy502 --
  {
    const s502 = await stand('502');
    const ms = await open();
    const st = await state();
    const play = await toTitle();
    await closeStand(s502);
    report('proxy502', st.phase === 'ready' && st.offline && play.ok && play.failedData === 0, { pageToReadyMs: ms, ...st, playToTitleMs: play.ms, failedData: play.failedData });
  }

  // ----------------------------------------------------------- silent --
  {
    const quiet = await stand('silent');
    const socks = new Set(); quiet.on('connection', s => { socks.add(s); s.on('close', () => socks.delete(s)); });
    // the service worker answers first for 30 s after the last failure: past that, the wait is measured
    await page.waitForTimeout(31000);
    const ms = await open();
    await page.waitForFunction(() => window.__nfsu2.offline(), null, { timeout: 10000 }).catch(() => {});   // the server check's own wait
    const st = await state();
    for (const s of socks) s.destroy();
    await new Promise(r => quiet.close(r));
    report('silent', st.phase === 'ready' && st.offline && ms >= 3500 && ms < 20000, { pageToReadyMs: ms, ...st });
  }

  // ------------------------------------------------------------- back --
  {
    await startServer();
    await page.waitForTimeout(31000);                                  // past the service worker's offline window
    const ms = await open();
    await page.waitForFunction(() => !window.__nfsu2.offline(), null, { timeout: 20000 }).catch(() => {});
    const st = await state();
    report('back', st.phase === 'ready' && !st.offline && st.sub === 'Press Play to start.', { pageToReadyMs: ms, ...st });
  }

  // ----------------------------------------------------------- deploy --
  {
    const next = deployed(), before = await loadedRev();
    await stopServer();
    const t0 = Date.now();
    await open();
    await page.waitForFunction(() => window.__nfsu2.offline(), null, { timeout: 10000 }).catch(() => {});
    const copies = { rev: await loadedRev(), ...await state() };
    await startServer(next.wasm);
    // the start page asks the server at once when the browser says it is online (players.ts), else within 15 s
    await page.evaluate(() => window.dispatchEvent(new Event('online')));
    const got = await page.waitForFunction(r => window.__nfsu2?.phase === 'ready' && window.__nfsu2.checks().find(c => c.id === 'module')?.label.endsWith(r),
      next.rev, { timeout: 60000, polling: 250 }).then(() => true, () => false);
    const ms = Date.now() - t0;
    const st = { rev: await loadedRev(), ...await state(), line: await page.evaluate(() => document.querySelector('#newer').hidden ? null : document.querySelector('#newer').textContent) };
    report('deploy', copies.rev === before && copies.offline && got && st.rev === next.rev && !st.offline && st.controlled &&
      /reloaded for the new version/.test(st.line ?? '') && ms < 30000, { before, copies, msFromCopies: ms, after: st });
  }
} catch (e) {
  report('run', false, String(e?.stack ?? e));
} finally {
  await pc.close();
  await stopServer();
}
const failed = results.filter(r => !r.ok);
console.log(`${results.length - failed.length}/${results.length} passed${failed.length ? `; failed: ${failed.map(r => r.step).join(', ')}` : ''}`);
if (!failed.length) fs.rmSync(OUT, { recursive: true, force: true });
process.exit(failed.length ? 1 : 0);
