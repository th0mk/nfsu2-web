// End to end in Chrome over TLS, as a friend would reach a VPS: the start
// page, the security of the page (secure context, cross-origin isolation, no
// CSP violations or policy warnings, nothing machine-specific on the page),
// Play, and the game up to its title screen or (--menu, with the real input
// and renderer of the integrated app) the main menu, with a screenshot.
//
//   node web/tools/deploy-e2e.mjs --out <dir> [--mode tls|proxy|both|docker|one-origin] [--menu] [--headed]
//        [--data "<game folder>"] [--port 8527] [--http-port 8627] [--server <binary>]
//        [--image <name>] [--module <dir>] [--d3dx <d3dx9_26.dll>] [--data-mw "<MW folder>"]
//        [--game ug2|mw] (the game's title and menu screens) [--help] (this text and those)
// --server: another server than web/tools/server-cmd.mjs's, such as the nfsu2-server of an unpacked bundle
// (web/deploy/pack.sh), with the page and the module of the folder it is in (NFSU2_REPO).
// --proxy-cmd "<command>": in mode proxy, this command is the proxy instead of the Cloudflare stand-in
// (a real Caddy with the example Caddyfile's directives, say); it must serve the test certificate on
// 127.0.0.1:<port> and forward to 127.0.0.1:<http-port>.  It is stopped at the end.
// CHROME_ARGS: more of Chrome's switches, space-separated (WSL's Chrome has SwiftShader only: its WebGPU needs
// the switches docs/web name), as web/tools/run-suite.mjs takes them.
//
// mode tls:    the server with --tls-cert <test certificate>   (Let's Encrypt / Origin CA on a VPS)
// mode proxy:  the server with --behind-proxy, and web/test/cf-sim.mjs in front (Cloudflare)
// mode docker: the Docker stack, web/deploy/compose.yml (the server's image and Caddy), with the test
//              certificate in Caddy instead of Let's Encrypt, its HTTPS on 127.0.0.1:<port> and the server on
//              127.0.0.1:<http-port> of the Docker host (with Colima or Docker Desktop: their Linux VM, which
//              forwards <port> to this Mac); not in "both".  --image <name> (default nfsu2-web:dev, from
//              docker build -f web/server-rs/Dockerfile -t nfsu2-web:dev .), --module <dir> (nfsu2.wasm + nfsu2.json;
//              default out/web; Most Wanted's nfsmw.wasm + nfsmw.json, default out/web/mw).
//              Needs docker compose; the stack is removed at the end.
// Chrome reaches https://nfsu2.test:<port>/ (mapped to 127.0.0.1) and accepts
// the test certificate by its key (--ignore-certificate-errors-spki-list), so
// the page is a real secure origin.  Nothing listens beyond 127.0.0.1 except
// the server's own TLS port in mode tls.  Exit 0 = every check passed.
//
// --game mw: Most Wanted's server (--game mw), its folder (--data, or NFSMW_DATA: the folder with speed.exe), its
// module (out/web/mw or --module) and page, all under /mw (PLAN D-38): https://nfsu2.test:<port>/mw/.  A module
// built with the D3DX oracle ("oracle":1 in its json: Most Wanted boots with no other yet) is a dev build that a
// server serves only with --dev (PLAN D-09): the run then starts the server with --dev, says so, and gives the page
// the DLL (--d3dx <d3dx9_26.dll>: into the page's save folder, NFSMW_D3DX=/save/d3dx9_26.dll by the address's env=,
// which only a --dev server's page takes).  In docker mode the server runs Most Wanted as compose.mw.yml has it
// (--game mw, NFSU2_WASM nfsmw.wasm), in a project of its own, without the tunnel.
//
// mode one-origin (TRACKER M11-24): both games under one host name, as the launch site has them: a proxy on
// http://localhost:<port> (web/test/cf-sim.mjs startRouteSim) sends / and /u2/ to an Underground 2 server
// (--base-path localhost=/u2) and /mw/ to a Most Wanted server, whose /mw/vault goes on to the Underground 2 server's
// vault (NFSU2_VAULT_FORWARD, the box-to-box credential made up for the run).  The old address is the other loopback
// name, http://127.0.0.1:<port>: the proxy sends it to a second Underground 2 server at the root, with its own state
// folder and vault, as today's box serves nfsu2.xyz in the side-by-side days (PLAN U-27 (b)).  Plain HTTP: both names
// are secure contexts, and the hand-over's loopback rule is http://127.0.0.1 (M13-29).  Underground 2's folder is
// --data (NFSU2_DATA), Most Wanted's --data-mw (NFSMW_DATA); the servers on <http-port>, +1 and +2, each with
// --behind-proxy --host <its name>, as the boxes run them (the Dockerfile's CMD).  Checks: COOP and
// COEP on every path of both addresses; /u2/lan and /mw/lan reach their own server only; one vault on the new address,
// the Underground 2 server's (/u2/vault directly, /mw/vault only through the forwarding), none in Most Wanted's state
// folder, the old address's its own; /mw/vault 503 from the Most Wanted server once the Underground 2 server is
// stopped; the old address's CSP and the new one's hand-over CORS by the loopback rule; in Chrome, each page's worker
// at its own base (/u2/, /mw/), none at /; each server's client address the one the proxy forwards (X-Forwarded-For).
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import fs from 'node:fs';
import os from 'node:os';
import net from 'node:net';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { makeTestCerts, startCfSim, startRouteSim } from '../test/cf-sim.mjs';
import { serverCommand } from './server-cmd.mjs';
import { gameOf, usage, nothingFor } from './rig-game.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
// each game's screens, as the front end's log names them: the title Play reaches, the main menu of --menu (and the
// words before the name: UG2's log says which package is on top, Most Wanted's each screen that comes up,
// runtime/mw/fe_mw.c), where its routes are, its module's name, its folder's default and its page's save folder (the
// browser's storage, web/src/games/<id>/storage.ts SAVE_DIR: the game's /save)
const GAMES_OF = {
  ug2: { title: 'UG_LS_Splash.fng', menu: 'GarageMain.fng', line: 'top package ', base: '', module: 'nfsu2', moduleDir: 'out/web', saves: 'save',
    data: process.env.NFSU2_DATA || path.join(os.homedir(), 'projects/nsfu2/Need for Speed - Underground 2') },
  mw: { title: 'MW_LS_Splash.fng', menu: 'MainMenu.fng', line: 'screen ', base: '/mw', module: 'nfsmw', moduleDir: 'out/web/mw', saves: 'mw-save',
    data: process.env.NFSMW_DATA || '' },
};
const MODE = arg('--mode', 'both');
const GAME = gameOf(process.argv.slice(2)), SCREENS = GAMES_OF[GAME];
if (process.argv.includes('--help')) usage(import.meta.url, GAME, 'Screens', SCREENS ? [`title: ${SCREENS.title}`, `--menu: ${SCREENS.menu}`] : []);
if (!SCREENS) nothingFor(GAME, 'no screens');
const OUT = path.resolve(arg('--out', 'out/web/deploy-e2e'));
const MODES = MODE === 'both' ? ['tls', 'proxy'] : [MODE];
const MENU = process.argv.includes('--menu');
const DATA = arg('--data', SCREENS.data);
const P1 = +arg('--port', 8527), P2 = +arg('--http-port', 8627);
const SERVER = arg('--server', '') && path.resolve(arg('--server'));
const PROXY_CMD = arg('--proxy-cmd', '');
const IMAGE = arg('--image', 'nfsu2-web:dev');
const MODULE = path.resolve(arg('--module', path.join(REPO, SCREENS.moduleDir)));
const D3DX = arg('--d3dx', '') && path.resolve(arg('--d3dx'));
const BASE = SCREENS.base;
if (!DATA) { console.error(`--game ${GAME}: no game folder (--data, or NFSMW_DATA)`); process.exit(2); }
fs.mkdirSync(OUT, { recursive: true });
const certs = makeTestCerts(path.join(OUT, 'certs'));
const results = [];
const report = (step, ok, evidence) => {
  results.push({ step, ok, evidence });
  console.log(`${ok ? 'PASS' : 'FAIL'} ${step}: ${typeof evidence === 'string' ? evidence : JSON.stringify(evidence)}`);
};
const machine = [os.hostname(), os.hostname().replace(/\.local$/, ''), os.homedir(),
  ...Object.values(os.networkInterfaces()).flat().filter(a => a && !a.internal).map(a => a.address)];
const chromeArgs = () => (process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ').filter(Boolean) : []);
/** whether a game's module json says it is a dev build with the D3DX oracle (web/build.sh "oracle":1) */
const oracleIn = (dir, name) => { try { return !!JSON.parse(fs.readFileSync(path.join(dir, `${name}.json`), 'utf8')).oracle; } catch { return false; } };
const ORACLE = oracleIn(MODULE, SCREENS.module);
if (ORACLE && !D3DX) { console.error(`${path.join(MODULE, SCREENS.module)}.json: a dev build with the D3DX oracle: give its DLL, --d3dx <d3dx9_26.dll>`); process.exit(2); }
/** the game's server options: its --game and module, and --dev for a dev build with the oracle (which a server
 *  serves with --dev only) */
const gameArgs = () => GAME === 'ug2' ? [] : ['--game', GAME, '--wasm', path.join(MODULE, `${SCREENS.module}.wasm`), ...(ORACLE ? ['--dev'] : [])];

async function startServer(args, { label = args.includes('--behind-proxy') ? 'proxy' : 'tls', data = DATA, env = {}, state = path.join(OUT, `state-${label}`) } = {}) {
  const c = spawn(...serverCommand(['--data', data, '--state', state, '--access-log', ...args], SERVER || undefined),
    { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, ...(SERVER ? { NFSU2_REPO: path.dirname(SERVER) } : {}), ...env } });
  const log = fs.createWriteStream(path.join(OUT, `server-${label}.log`));
  c.stdout.pipe(log); c.stderr.pipe(log);
  const exited = new Promise(r => c.once('exit', r));
  await new Promise((res, rej) => {
    let out = '';
    const t = setTimeout(() => rej(new Error('server did not start')), 60000);
    c.stdout.on('data', d => { out += d; if (out.includes('  access    :')) { clearTimeout(t); res(); } });
    c.on('exit', code => { clearTimeout(t); rej(new Error(`server exited (${code}): ${out}`)); });
  });
  return { state, stop: () => { if (c.exitCode === null && c.signalCode === null) c.kill('SIGTERM'); return exited; } };
}

// the shipped stack, but Caddy serves the test certificate on 127.0.0.1:<port> only, without its http -> https
// port, with a Caddyfile of its own (the proxy and header -Server; compose.yml's other directives are not in it);
// Most Wanted's server as compose.mw.yml runs it (its tunnel's services left out)
async function startDocker() {
  const override = path.join(OUT, 'compose.e2e.yml');
  const mw = GAME === 'mw' ? `  server:
    command: ["--game", "mw", "--behind-proxy"${ORACLE ? ', "--dev"' : ''}]
    environment:
      NFSU2_WASM: /module/nfsmw.wasm
` : '';
  fs.writeFileSync(override, `services:
  caddy:
    configs: !override [{ source: caddyfile-e2e, target: /etc/caddy/Caddyfile }]
    volumes: ["${certs.dir}:/certs:ro"]
${mw}configs:
  caddyfile-e2e:
    content: |
      {
      	admin off
      	default_bind 127.0.0.1
      	https_port ${P1}
      	auto_https disable_redirects
      }
      nfsu2.test {
      	tls /certs/${path.basename(certs.cert)} /certs/${path.basename(certs.key)}
      	reverse_proxy 127.0.0.1:${P2}
      	header -Server
      }
`);
  const env = { ...process.env, NFSU2_HOST: 'nfsu2.test', NFSU2_DATA: DATA, NFSU2_MODULE: MODULE, NFSU2_IMAGE: IMAGE, NFSU2_HTTP_PORT: String(P2) };
  const project = GAME === 'ug2' ? 'nfsu2-e2e' : `nfs${GAME}-e2e`;
  const compose = (...a) => execFileSync('docker', ['compose', '-p', project, '-f', path.join(REPO, 'web/deploy/compose.yml'), '-f', override, ...a],
    { env, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  // --wait: until the image's health check sees the module served
  compose('up', '-d', '--wait', '--wait-timeout', '120');
  return { stop: async () => {
    fs.writeFileSync(path.join(OUT, 'server-docker.log'), compose('logs', '--no-color', 'server', 'caddy'));
    compose('down', '--volumes');
  } };
}

/** Chrome for these pages: secure by the test certificate's key, nfsu2.test at this machine */
const launch = () => chromium.launch({ ...pickBrowser(), headless: !process.argv.includes('--headed'), args: [
  '--enable-unsafe-webgpu', `--host-resolver-rules=MAP nfsu2.test 127.0.0.1`,
  `--ignore-certificate-errors-spki-list=${certs.spki.join(',')}`, ...chromeArgs()] });

/** the D3DX oracle's DLL into the page's save folder (the game's /save), where NFSMW_D3DX=/save/d3dx9_26.dll reads it */
async function giveDll(page) {
  const b64 = fs.readFileSync(D3DX).toString('base64');
  await page.evaluate(async ([b, saves]) => {
    const bytes = Uint8Array.from(atob(b), c => c.charCodeAt(0));
    const d = await (await navigator.storage.getDirectory()).getDirectoryHandle(saves, { create: true });
    const w = await (await d.getFileHandle('d3dx9_26.dll', { create: true })).createWritable();
    await w.write(bytes); await w.close();
  }, [b64, SCREENS.saves]);
}

async function run(mode) {
  const server = mode === 'tls'
    ? await startServer(['--port', String(P1), '--http-port', '0', '--tls-cert', certs.cert, '--tls-key', certs.key, ...gameArgs()])
    : mode === 'docker' ? await startDocker()
    : await startServer(['--behind-proxy', '--http-port', String(P2), '--host', 'nfsu2.test', ...gameArgs()]);
  if (ORACLE) report(`${mode}: the module is a dev build with the D3DX oracle: served with --dev`, true, 'PLAN D-09: no oracle build is served without --dev');
  let sim = null;
  if (mode === 'proxy' && PROXY_CMD) {
    const pc = spawn('/bin/sh', ['-c', `exec ${PROXY_CMD}`], { stdio: ['ignore', 'pipe', 'pipe'] });
    const plog = fs.createWriteStream(path.join(OUT, 'proxy.log'));
    pc.stdout.pipe(plog); pc.stderr.pipe(plog);
    const t0 = Date.now();
    while (Date.now() - t0 < 20000) {
      const up = await new Promise(r => { const so = net.connect(P1, '127.0.0.1', () => { so.destroy(); r(true); }); so.on('error', () => r(false)); });
      if (up) break;
      await new Promise(r => setTimeout(r, 250));
    }
    sim = { close: () => new Promise(r => { pc.once('exit', r); pc.kill('SIGTERM'); }) };
  } else if (mode === 'proxy') {
    sim = startCfSim({ port: P1, target: P2, certs, fakeClientIp: '198.51.100.7' });
    await sim.ready;
  }
  const browser = await launch();
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 800 } });
  const page = await ctx.newPage();
  const consoleLines = [];
  page.on('console', m => consoleLines.push(`${m.type()} ${m.text()}`));
  page.on('pageerror', e => consoleLines.push(`PAGEERROR ${e}`));
  await page.addInitScript(() => {
    window.__cspv = [];
    document.addEventListener('securitypolicyviolation', e => window.__cspv.push(`${e.violatedDirective} ${e.blockedURI}`));
  });
  const origin = `https://nfsu2.test:${P1}`;
  // (a dev build with the oracle: the DLL's path by the address, which a --dev server's page takes)
  const at = `${origin}${BASE}/${ORACLE ? '?env=NFSMW_D3DX=/save/d3dx9_26.dll' : ''}`;
  const t0 = Date.now();
  try {
    // a production server: the page takes no settings from its address (the audit's PAGE-1), so the test's own
    // (no movies, the front-end log the checks below read, free roam answered: the first Play would ask) go into
    // this fresh browser's settings.ini, as a player's would
    const resp = await page.goto(at);
    const h = resp.headers();
    report(`${mode}: document headers`, !!h['content-security-policy'] && h['cross-origin-embedder-policy'] === 'require-corp' &&
      h['cross-origin-opener-policy'] === 'same-origin' && h['strict-transport-security'] === 'max-age=31536000' && /no-store/.test(h['cdn-cache-control'] ?? ''),
      { url: `${BASE}/`, hsts: h['strict-transport-security'], coep: h['cross-origin-embedder-policy'], cache: h['cache-control'], server: h.server ?? null });
    await page.waitForFunction(() => ['ready', 'error'].includes(window.__nfsu2?.phase), null, { timeout: 180000, polling: 250 });
    const st = await page.evaluate(() => ({
      phase: window.__nfsu2.phase, secure: isSecureContext, isolated: crossOriginIsolated, sab: typeof SharedArrayBuffer,
      protocol: location.protocol, host: location.host,
      checks: [...document.querySelectorAll('#checks li')].map(li => `${li.dataset.id}:${li.dataset.state}`),
      text: document.body.innerText, setupLink: !!document.querySelector('#setup-note:not([hidden])'),   // (the page has no setup note any more)
    }));
    report(`${mode}: secure and cross-origin isolated`, st.secure && st.isolated && st.sab === 'function' && st.protocol === 'https:',
      { isSecureContext: st.secure, crossOriginIsolated: st.isolated, SharedArrayBuffer: st.sab, origin: `${st.protocol}//${st.host}` });
    report(`${mode}: start page ready`, st.phase === 'ready' && !st.checks.some(c => c.endsWith(':fail')), { phase: st.phase, checks: st.checks, ms: Date.now() - t0 });
    const leaked = machine.filter(m => m && st.text.includes(m));
    report(`${mode}: nothing machine-specific on the page`, leaked.length === 0 && !st.setupLink, { leaked, setupLinkShown: st.setupLink });
    await page.screenshot({ path: path.join(OUT, `${mode}-start.png`) });
    await page.evaluate(async saves => {
      const d = await (await navigator.storage.getDirectory()).getDirectoryHandle(saves, { create: true });
      const w = await (await d.getFileHandle('settings.ini', { create: true })).createWritable();
      await w.write('# NFSU2 settings, read at every launch.\nmovies = off\nNFSU2_FNG_LOG = 1\nroam = off\n'); await w.close();
    }, SCREENS.saves);
    if (ORACLE) await giveDll(page);
    // Play
    await page.evaluate(() => window.__nfsu2.log.clear());
    await page.click('#play');
    const tp = Date.now();
    const seen = t => page.evaluate(w => window.__nfsu2.log.tail(600).some(l => l.includes(w)), `${SCREENS.line}${t}`);
    // (or the game's end, which no title follows)
    await page.waitForFunction(w => window.__nfsu2.log.tail(400).some(l => l.includes(w) || l.includes('[app] the game exited')),
      `${SCREENS.line}${SCREENS.title}`, { timeout: 300000, polling: 200 });
    const ended = await page.evaluate(() => window.__nfsu2.log.tail(400).find(l => l.includes('[app] the game exited')) ?? null);
    if (ended && !await seen(SCREENS.title)) throw new Error(`the game ended before its title screen: ${ended}`);
    report(`${mode}: Play -> the game's title screen`, true, { screen: SCREENS.title, ms: Date.now() - tp });
    if (MENU) {
      // Enter through the title and the profile prompt to the main menu, with real key events
      const tm = Date.now();
      let reached = false;
      while (Date.now() - tm < 120000 && !reached) {
        await page.keyboard.press('Enter');
        await page.waitForTimeout(1500);
        reached = await seen(SCREENS.menu);
      }
      await page.waitForTimeout(2500);
      await page.screenshot({ path: path.join(OUT, `${mode}-menu.png`) });
      const gpu = await page.evaluate(() => window.__nfsu2.log.tail(2000).filter(l => /renderer attached|gpu|WebGPU/i.test(l)).slice(-4));
      report(`${mode}: main menu with the renderer`, reached, { screen: SCREENS.menu, ms: Date.now() - tm, screenshot: `${mode}-menu.png`, renderer: gpu });
    } else await page.screenshot({ path: path.join(OUT, `${mode}-title.png`) });
    await page.evaluate(() => window.__nfsu2.session?.terminate?.());
    await page.waitForTimeout(1000);
    const cspv = await page.evaluate(() => window.__cspv);
    const policy = consoleLines.filter(l => /Content Security Policy|Refused to|Permissions-Policy|Permissions Policy|Unrecognized feature|Cross-Origin|COEP|blocked/i.test(l));
    report(`${mode}: no CSP violation, no policy warning, no page error`, cspv.length === 0 && policy.length === 0 && !consoleLines.some(l => l.startsWith('PAGEERROR')),
      { violations: cspv, policyLines: policy.slice(0, 5), pageErrors: consoleLines.filter(l => l.startsWith('PAGEERROR')).slice(0, 3) });
  } catch (e) {
    report(`${mode}: run`, false, String(e).slice(0, 400));
    await page.screenshot({ path: path.join(OUT, `${mode}-failure.png`) }).catch(() => {});
  } finally {
    fs.writeFileSync(path.join(OUT, `${mode}-console.log`), consoleLines.join('\n'));
    // (the game's own lines too: what a failed start says)
    const game = await page.evaluate(() => window.__nfsu2?.log?.tail(3000) ?? []).catch(() => []);
    fs.writeFileSync(path.join(OUT, `${mode}-game.log`), game.join('\n'));
    await browser.close();
    if (sim) await sim.close();
    await server.stop();
  }
}

// ---- one-origin: both games under one host name (TRACKER M11-24) ----

/** an answer through the proxy: status, headers, body (a raw request: the Host as given, nothing cleaned) */
function ask(port, { method = 'GET', p, host, headers = {}, body = null }) {
  return new Promise(res => {
    const so = net.connect(port, '127.0.0.1');
    const chunks = [];
    const t = setTimeout(() => { so.destroy(); }, 30000);
    so.on('connect', () => {
      const b = body === null ? null : Buffer.from(body);
      so.write(`${method} ${p} HTTP/1.1\r\nHost: ${host}\r\nConnection: close\r\n${Object.entries(headers).map(([k, v]) => `${k}: ${v}\r\n`).join('')}` +
        `${b ? `Content-Length: ${b.length}\r\n` : ''}\r\n`);
      if (b) so.write(b);
    });
    so.on('data', d => chunks.push(d));
    so.on('error', () => {});
    so.on('close', () => {
      clearTimeout(t);
      const buf = Buffer.concat(chunks), end = buf.indexOf('\r\n\r\n');
      if (end < 0) return res({ status: 0, headers: {}, body: '' });
      const [line, ...rest] = buf.subarray(0, end).toString('latin1').split('\r\n');
      const h = {};
      for (const l of rest) { const i = l.indexOf(':'); h[l.slice(0, i).trim().toLowerCase()] = l.slice(i + 1).trim(); }
      let b = buf.subarray(end + 4);
      if (/chunked/i.test(h['transfer-encoding'] ?? '')) {
        const out = []; let i = 0;
        while (i < b.length) { const nl = b.indexOf('\r\n', i); const n = parseInt(b.subarray(i, nl).toString(), 16); if (!n) break; out.push(b.subarray(nl + 2, nl + 2 + n)); i = nl + 2 + n + 2; }
        b = Buffer.concat(out);
      }
      res({ status: +line.split(' ')[1], headers: h, body: b.toString('latin1') });
    });
  });
}

/** a client's text frame (masked, as a client's must be) */
function wsText(text) {
  const body = Buffer.from(text), mask = crypto.randomBytes(4);
  const head = body.length < 126 ? Buffer.from([0x81, 0x80 | body.length]) : Buffer.from([0x81, 0x80 | 126, body.length >> 8, body.length & 255]);
  return Buffer.concat([head, mask, Buffer.from(body.map((b, i) => b ^ mask[i & 3]))]);
}

/** a /lan WebSocket through the proxy, a player once the switch's welcome came (its hello sent), or the refusal's
 *  status */
function lanOpen(port, host, p, origin) {
  return new Promise(res => {
    const so = net.connect(port, '127.0.0.1');
    let got = '';
    const t = setTimeout(() => { so.destroy(); res({ status: 0 }); }, 10000);
    so.on('connect', () => so.write(`GET ${p} HTTP/1.1\r\nHost: ${host}\r\nOrigin: ${origin}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
      `Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\n\r\n`));
    so.on('data', d => {
      got += d.toString('latin1');
      const status = +got.split(' ')[1];
      if (status !== 101 && got.includes('\r\n\r\n')) { clearTimeout(t); so.destroy(); res({ status }); }
      // (the welcome: a text frame after the 101's head)
      else if (status === 101 && got.includes('welcome') && !so.said) {
        so.said = true;
        so.write(wsText(JSON.stringify({ t: 'hello', name: 'one-origin' })));
        clearTimeout(t); res({ status, close: () => so.destroy() });
      }
    });
    so.on('error', () => { clearTimeout(t); res({ status: 0 }); });
  });
}

/** every file under dir (relative names) */
const filesUnder = dir => fs.existsSync(dir) ? fs.readdirSync(dir, { recursive: true }).map(String).filter(f => fs.statSync(path.join(dir, f)).isFile()) : [];

async function runOneOrigin() {
  const DATA_MW = arg('--data-mw', process.env.NFSMW_DATA || '');
  const MODULE_MW = path.resolve(arg('--module-mw', path.join(REPO, 'out/web/mw')));
  const ORACLE_MW = oracleIn(MODULE_MW, 'nfsmw');
  if (!DATA_MW) { console.error('mode one-origin: Most Wanted\'s folder too (--data-mw, or NFSMW_DATA)'); process.exit(2); }
  if (ORACLE_MW && !D3DX) { console.error(`${MODULE_MW}/nfsmw.json: a dev build with the D3DX oracle: give its DLL, --d3dx <d3dx9_26.dll>`); process.exit(2); }
  const dataUg2 = arg('--data', GAMES_OF.ug2.data);
  const [PU, PM, PO] = [P2, P2 + 1, P2 + 2];
  const dir = path.join(OUT, 'one-origin');
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true });
  // the box-to-box credential, made up for this run (never written anywhere but these two servers' environment)
  const boxKey = crypto.randomBytes(32).toString('base64url');
  const NEW = `localhost:${P1}`, OLD = `127.0.0.1:${P1}`, newOrigin = `http://${NEW}`, oldOrigin = `http://${OLD}`;
  // (each started one is stopped at the end, whatever fails on the way)
  const servers = {};
  let sim = null, browser = null;
  try {
    // (each behind the proxy as the boxes run it, the Dockerfile's CMD: --behind-proxy and the name it answers)
    const behind = name => ['--behind-proxy', '--host', name];
    servers.u2 = await startServer(['--http-port', String(PU), ...behind('localhost'), '--base-path', 'localhost=/u2'],
      { label: 'one-origin-u2', data: dataUg2, state: path.join(dir, 'state-u2'), env: { NFSU2_VAULT_BOX_KEY: boxKey } });
    servers.mw = await startServer(['--game', 'mw', '--http-port', String(PM), ...behind('localhost'), '--wasm', path.join(MODULE_MW, 'nfsmw.wasm'),
      ...(ORACLE_MW ? ['--dev'] : [])],
      { label: 'one-origin-mw', data: DATA_MW, state: path.join(dir, 'state-mw'),
        env: { NFSU2_VAULT_FORWARD: `http://127.0.0.1:${PU}/vault`, NFSU2_VAULT_BOX_KEY: boxKey } });
    servers.old = await startServer(['--http-port', String(PO), ...behind('127.0.0.1')],
      { label: 'one-origin-old', data: dataUg2, state: path.join(dir, 'state-old') });
    if (ORACLE_MW) report('one-origin: Most Wanted\'s module is a dev build with the D3DX oracle: its server runs with --dev', true, 'PLAN D-09');
    // the edge: one name's paths on two servers, the other name on a third
    sim = startRouteSim({ port: P1, fakeClientIp: '198.51.100.7',
      route: (name, p) => name === '127.0.0.1' ? PO : name !== 'localhost' ? null : (p === '/mw' || p.startsWith('/mw/')) ? PM : PU });
    await sim.ready;
    browser = await launch();
    // 1. COOP and COEP on every path of both addresses, whatever the answer (404s and redirects too)
    const paths = {
      [NEW]: ['/', '/robots.txt', '/u2', '/u2/', '/u2/index.html', '/u2/main.js', '/u2/game.json', '/u2/data-manifest', '/u2/players', '/u2/nothing',
        '/u2/vault', '/mw', '/mw/', '/mw/index.html', '/mw/main.js', '/mw/game.json', '/mw/data-manifest', '/mw/players', '/mw/nothing', '/mw/vault',
        '/game.json', '/players', '/nothing', '/MW/', '/u2x'],
      [OLD]: ['/', '/index.html', '/main.js', '/game.json', '/data-manifest', '/players', '/robots.txt', '/nothing', '/u2/', '/mw/'],
    };
    const missing = [], statuses = {};
    for (const [host, ps] of Object.entries(paths)) {
      for (const p of ps) {
        const r = await ask(P1, { p, host });
        statuses[`${host.split(':')[0]}${p}`] = r.status;
        if (r.headers['cross-origin-opener-policy'] !== 'same-origin' || r.headers['cross-origin-embedder-policy'] !== 'require-corp') missing.push(`${host}${p} (${r.status})`);
      }
    }
    report('one-origin: COOP and COEP on every path of both addresses', missing.length === 0, { missing, statuses });
    const want = { 'localhost/': 404, 'localhost/u2': 308, 'localhost/u2/': 200, 'localhost/mw': 308, 'localhost/mw/': 200, 'localhost/u2/game.json': 200,
      'localhost/mw/game.json': 200, 'localhost/game.json': 404, 'localhost/robots.txt': 200, '127.0.0.1/': 200, '127.0.0.1/game.json': 200, '127.0.0.1/mw/': 404 };
    const off = Object.entries(want).filter(([k, v]) => statuses[k] !== v).map(([k, v]) => `${k}: ${statuses[k]}, not ${v}`);
    report('one-origin: each base where it belongs (the new address\'s root page is M13-13\'s, 404 until then)', off.length === 0, off.length ? off : 'as expected');
    // 2. each game's /lan reaches its own server only
    const count = async p => JSON.parse((await ask(P1, { p, host: NEW })).body || '{}').players;
    const before = [await count('/u2/players'), await count('/mw/players')];
    const u = await lanOpen(P1, NEW, '/u2/lan', newOrigin);
    await new Promise(r => setTimeout(r, 300));
    const afterU = [await count('/u2/players'), await count('/mw/players')];
    const m = await lanOpen(P1, NEW, '/mw/lan', newOrigin);
    await new Promise(r => setTimeout(r, 300));
    const afterM = [await count('/u2/players'), await count('/mw/players')];
    const root = await lanOpen(P1, NEW, '/lan', newOrigin);
    const old = await lanOpen(P1, OLD, '/lan', oldOrigin);
    await new Promise(r => setTimeout(r, 300));
    const oldCount = JSON.parse((await ask(P1, { p: '/players', host: OLD })).body || '{}').players;
    u.close?.(); m.close?.(); old.close?.();
    report('one-origin: /u2/lan and /mw/lan reach their own server only', u.status === 101 && m.status === 101 && root.status === 404 && old.status === 101 &&
      String(before) === '0,0' && String(afterU) === '1,0' && String(afterM) === '1,1' && oldCount === 1,
      { u2: u.status, mw: m.status, rootLan: root.status, oldLan: old.status, players: { before, afterU, afterM, old: oldCount } });
    // 3. one vault on the new address, the Underground 2 server's: /u2/vault directly, /mw/vault only forwarded; the
    // old address's its own; none in Most Wanted's state folder
    const tok = () => crypto.randomBytes(32).toString('base64url');
    const page = (host, origin) => ({ Origin: origin, 'Sec-Fetch-Site': 'same-origin', 'Sec-Fetch-Mode': 'cors' });
    const vault = (host, p, token, o = {}) => ask(P1, { p, host, ...o, headers: { ...page(host, host === NEW ? newOrigin : oldOrigin), Authorization: `Bearer ${token}`, ...(o.headers ?? {}) } });
    const tu = tok(), tm = tok();
    const putU = await vault(NEW, '/u2/vault', tu, { method: 'PUT', headers: { 'If-None-Match': '*' }, body: 'sealed u2 box' });
    const putM = await vault(NEW, '/mw/vault', tm, { method: 'PUT', headers: { 'If-None-Match': '*' }, body: 'sealed mw box' });
    const getM = await vault(NEW, '/mw/vault', tm);
    const getMviaU = await vault(NEW, '/u2/vault', tm);
    const getUviaM = await vault(NEW, '/mw/vault', tu);
    const getOld = await vault(OLD, '/vault', tu);
    const keptU = filesUnder(path.join(dir, 'state-u2', 'vault')).length, keptOld = filesUnder(path.join(dir, 'state-old', 'vault')).length;
    const mwFiles = filesUnder(path.join(dir, 'state-mw'));
    report('one-origin: one vault on the new address, the Underground 2 server\'s; Most Wanted\'s through the forwarding only',
      putU.status === 201 && putM.status === 201 && getM.status === 200 && getM.body === 'sealed mw box' && getMviaU.status === 200 &&
      getUviaM.status === 200 && getOld.status === 404 && keptU > 0 && !mwFiles.some(f => /vault/i.test(f)),
      { putU2: putU.status, putMw: putM.status, getMw: getM.status, mwBoxAtU2: getMviaU.status, u2BoxAtMw: getUviaM.status, oldAddress: getOld.status,
        files: { u2Vault: keptU, oldVault: keptOld }, mwState: mwFiles.filter(f => !f.startsWith('log/')) });
    // 4. the M13-29 loopback rule: the old address's CSP (connect-src 'self' until the launch), the new address's
    // hand-over CORS for the old one's origin only
    const csp = (await ask(P1, { p: '/', host: OLD })).headers['content-security-policy'] ?? '';
    const pre = o => ask(P1, { method: 'OPTIONS', p: '/u2/covers', host: NEW, headers: { Origin: o, 'Access-Control-Request-Method': 'PUT',
      'Access-Control-Request-Headers': 'authorization, if-none-match' } });
    const ok = await pre(oldOrigin), foreign = await pre('http://evil.example');
    report('one-origin: the old address\'s CSP and the new address\'s hand-over CORS by the loopback rule',
      /connect-src 'self'[;]/.test(csp) && ok.headers['access-control-allow-origin'] === oldOrigin && /origin/i.test(ok.headers.vary ?? '') &&
      !foreign.headers['access-control-allow-origin'] && foreign.status === 403,
      { oldCsp: (/connect-src [^;]*/.exec(csp) ?? [''])[0], preflight: [ok.status, ok.headers['access-control-allow-origin']], foreign: foreign.status });
    // 5. each page's worker at its own base, none at the root (the pages register theirs once their module loaded)
    const ctx = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const pg = await ctx.newPage();
    const scopes = [];
    for (const [p, q] of [['/u2/', ''], ['/mw/', ORACLE_MW ? '?env=NFSMW_D3DX=/save/d3dx9_26.dll' : '']]) {
      await pg.goto(`${newOrigin}${p}${q}`);
      await pg.waitForFunction(() => ['ready', 'error'].includes(window.__nfsu2?.phase), null, { timeout: 300000, polling: 250 });
      const iso = await pg.evaluate(() => crossOriginIsolated);
      const t0 = Date.now();
      let regs = [];
      while (Date.now() - t0 < 60000) {
        regs = await pg.evaluate(async () => (await navigator.serviceWorker.getRegistrations()).map(r => r.scope));
        if (regs.some(s => s.endsWith(p))) break;
        await pg.waitForTimeout(500);
      }
      scopes.push({ page: p, isolated: iso, phase: await pg.evaluate(() => window.__nfsu2.phase), registrations: regs });
    }
    const all = scopes.at(-1).registrations.map(s => s.replace(newOrigin, '')).sort();
    report('one-origin: each page\'s worker at its own base (/u2/, /mw/), none at /', JSON.stringify(all) === '["/mw/","/u2/"]' && scopes.every(s => s.isolated),
      { scopes: all, pages: scopes.map(s => ({ page: s.page, isolated: s.isolated, phase: s.phase })) });
    await ctx.close();
    // 6. the Underground 2 server stopped: /mw/vault 503 from the Most Wanted server (its "unreachable", at once:
    // nothing listens), the page's local saves its own business
    await servers.u2.stop();
    const t1 = Date.now();
    const down = await vault(NEW, '/mw/vault', tm);
    report('one-origin: the Underground 2 server stopped: /mw/vault 503 from the Most Wanted server', down.status === 503 &&
      down.headers['x-vault-unreachable'] === '1' && Date.now() - t1 < 6000, { status: down.status, unreachable: down.headers['x-vault-unreachable'] ?? null, ms: Date.now() - t1 });
    const mwAfter = filesUnder(path.join(dir, 'state-mw'));
    report('one-origin: Most Wanted\'s state folder holds no vault files', !mwAfter.some(f => /vault/i.test(f)) && !fs.existsSync(path.join(dir, 'state-mw', 'vault')),
      { files: mwAfter.filter(f => !f.startsWith('log/')) });
    // 7. the client's address each server sees (its access log): the one the proxy forwards (X-Forwarded-For), never
    // the proxy's own; the Most Wanted server's /vault forwarding reaches the Underground 2 server directly, not
    // through the proxy, and is left out
    await new Promise(r => setTimeout(r, 300));
    const seen = {};
    for (const name of ['u2', 'mw', 'old']) {
      const by = {};
      for (const l of fs.readFileSync(path.join(OUT, `server-one-origin-${name}.log`), 'utf8').split('\n')) {
        const m = /\[http\] (\S+) \S+ (\S+) /.exec(l);
        if (m && !(name === 'u2' && m[2].startsWith('/vault'))) by[m[1]] = (by[m[1]] ?? 0) + 1;
      }
      seen[name] = by;
    }
    report('one-origin: each server sees the address the proxy forwards (X-Forwarded-For), not the proxy\'s',
      Object.values(seen).every(by => Object.keys(by).length === 1 && by['198.51.100.7'] > 0), seen);
    report('one-origin: the proxy\'s requests by server', true, { u2: sim.stats.byPort[PU] ?? 0, mw: sim.stats.byPort[PM] ?? 0, old: sim.stats.byPort[PO] ?? 0 });
  } catch (e) {
    report('one-origin: run', false, String(e?.stack ?? e).slice(0, 600));
  } finally {
    await browser?.close();
    await sim?.close();
    for (const s of Object.values(servers)) await s.stop();
  }
}

if (MODE === 'one-origin') await runOneOrigin();
else for (const m of MODES) await run(m);
fs.writeFileSync(path.join(OUT, 'results.json'), JSON.stringify(results, null, 2));
const failed = results.filter(r => !r.ok).length;
console.log(`${results.length - failed}/${results.length} passed; ${OUT}`);
process.exit(failed ? 1 : 0);
