// End to end in Chrome over TLS, as a friend would reach a VPS: the start
// page, the security of the page (secure context, cross-origin isolation, no
// CSP violations or policy warnings, nothing machine-specific on the page),
// Play, and the game up to its title screen or (--menu, with the real input
// and renderer of the integrated app) the main menu, with a screenshot.
//
//   node web/tools/deploy-e2e.mjs --out <dir> [--mode tls|proxy|both|docker|two-domain] [--menu] [--headed]
//        [--data "<game folder>"] [--port 8527] [--http-port 8627] [--server <binary>]
//        [--image <name>] [--module <dir>] [--d3dx <d3dx9_26.dll>] [--data-mw "<MW folder>"] [--module-mw <dir>]
//        [--backup-u2 <a "Download a backup" zip>] [--saves-mw <a folder with NFS Most Wanted/<NAME>/<NAME>>]
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
// mode two-domain (the site plan of 2026-10-31; TRACKER M11-24's mode for it): each game on its own site, as the
// boxes have them: a proxy on http://<port> (web/test/cf-sim.mjs startRouteSim) sends the name nfsu2.localhost (for
// https://nfsu2.xyz) to an Underground 2 server at the root and nfsrevived.localhost (for https://nfsrevived.com) to a
// Most Wanted server (/mw/, and the site's page at /), whose /mw/vault goes on to the Underground 2 server's vault
// (NFSU2_VAULT_FORWARD, the box-to-box credential made up for the run).  Both servers with --site-redirects on, as the
// boxes run them.  Plain HTTP: Chrome takes *.localhost for this machine and as secure contexts.  Underground 2's
// folder is --data (NFSU2_DATA), Most Wanted's --data-mw (NFSMW_DATA); the servers on <http-port> and +1, each with
// --behind-proxy --host <its name>.  Checks: COOP and COEP on every path of both names; each game where it belongs,
// the site's page at nfsrevived's root, nfsu2/mw and under it to https://nfsrevived.com/mw/, nfsrevived/u2 and under it
// to https://nfsu2.xyz/ with the rest of the path, and no open redirect (only those fixed addresses); each game's /lan
// its own server's; one keyphrase, one vault: each game's box under its own token from the one phrase, Underground 2's
// at nfsu2/vault and Most Wanted's through nfsrevived/mw/vault, each readable through either name, none in Most
// Wanted's state folder; in Chrome the site's page (both links, its stylesheet and pictures, no CSP refusal),
// Underground 2's page as it is (no bar while its launch switch is off) with its worker at /, Most Wanted's with the
// bar (Underground 2's tab to https://nfsu2.xyz/) and its worker at /mw/; then in a fresh browser the phrase typed into
// each game's page restores that game's saves (--backup-u2: a real "Download a backup" zip of Underground 2's;
// --saves-mw: a folder of Most Wanted's profiles, a career seed's; made-up bytes without them); the passphrase link
// both ways: a phrase made on one game's page and typed into the other's asks 'Use this passphrase here too?' and,
// answered Send, puts that game's saves under it, the first game's box untouched (Underground 2's page asks only with
// its launch switch on: first as it is, which says there are no cloud saves, then on a page built from this checkout
// with the switch on, its server started again on it); /mw/vault 503 from the Most Wanted server once the
// Underground 2 server is stopped.  The redirects' addresses are checked as answers,
// never followed: nothing here reaches a real site.
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import fs from 'node:fs';
import os from 'node:os';
import net from 'node:net';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { makeTestCerts, startCfSim, startRouteSim } from '../test/cf-sim.mjs';
import { serverCommand } from './server-cmd.mjs';
import { gameOf, usage, nothingFor } from './rig-game.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
// each game's screens, as the front end's log names them: the title Play reaches (Most Wanted's on a 16:9 or wider
// canvas is its widescreen one, wideTitle: runtime/mw/widescreen_mw.c), the main menu of --menu (and the words before
// the name: UG2's log says which package is on top, Most Wanted's each screen that comes up, runtime/mw/fe_mw.c), where
// its routes are, its module's name, its folder's default and its page's save folder (the browser's storage,
// web/src/games/<id>/storage.ts SAVE_DIR: the game's /save)
const GAMES_OF = {
  ug2: { title: 'UG_LS_Splash.fng', menu: 'GarageMain.fng', line: 'top package ', base: '', module: 'nfsu2', moduleDir: 'out/web', saves: 'save',
    data: process.env.NFSU2_DATA || path.join(os.homedir(), 'projects/nsfu2/Need for Speed - Underground 2') },
  mw: { title: 'MW_LS_Splash.fng', wideTitle: 'WS_MW_LS_Splash.fng', menu: 'MainMenu.fng', line: 'screen ', base: '/mw',
    module: 'nfsmw', moduleDir: 'out/web/mw', saves: 'mw-save', data: process.env.NFSMW_DATA || '' },
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
    // either title (the 4:3 one or, on a wide canvas, Most Wanted's widescreen one) in its screen line
    const titles = [SCREENS.title, SCREENS.wideTitle].filter(Boolean).map(t => `${SCREENS.line}${t}`);
    const titleIn = () => page.evaluate(ws => { const t = window.__nfsu2.log.tail(600); return ws.find(w => t.some(l => l.includes(w))) ?? null; }, titles);
    // (or the game's end, which no title follows)
    await page.waitForFunction(ws => window.__nfsu2.log.tail(400).some(l => ws.some(w => l.includes(w)) || l.includes('[app] the game exited')),
      titles, { timeout: 300000, polling: 200 });
    const ended = await page.evaluate(() => window.__nfsu2.log.tail(400).find(l => l.includes('[app] the game exited')) ?? null);
    const titleLine = await titleIn();
    if (ended && !titleLine) throw new Error(`the game ended before its title screen: ${ended}`);
    report(`${mode}: Play -> the game's title screen`, true, { screen: titleLine ? titleLine.slice(SCREENS.line.length) : SCREENS.title,
      ms: Date.now() - tp });
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
    // Chrome's "Unrecognized feature" lines are left out: the Permissions-Policy header names features (bluetooth) for
    // the browsers that know them, and a browser that does not says so without a policy being broken
    const policy = consoleLines.filter(l => /Content Security Policy|Refused to|Permissions-Policy|Permissions Policy|Cross-Origin|COEP|blocked/i.test(l) &&
      !/Unrecognized feature/i.test(l));
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

// ---- two-domain: each game on its own site (the site plan of 2026-10-31) ----

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
        so.write(wsText(JSON.stringify({ t: 'hello', name: 'two-domain' })));
        clearTimeout(t); res({ status, close: () => so.destroy() });
      }
    });
    so.on('error', () => { clearTimeout(t); res({ status: 0 }); });
  });
}

/** every file under dir (relative names) */
const filesUnder = dir => fs.existsSync(dir) ? fs.readdirSync(dir, { recursive: true }).map(String).filter(f => fs.statSync(path.join(dir, f)).isFile()) : [];

/** vault.ts as a game's page bundles it (its own '@game': keyphrase derivation, seal's AAD) */
async function vaultOf(game) {
  const r = await esbuild.build({ entryPoints: [path.join(REPO, 'web/src/app/vault.ts')], bundle: true, format: 'esm', platform: 'neutral', write: false,
    alias: { '@game': path.join(REPO, 'web/src/games', game) }, logLevel: 'silent' });
  return import('data:text/javascript;base64,' + Buffer.from(r.outputFiles[0].text).toString('base64'));
}
/** zip.ts (unzip of a "Download a backup" zip) */
async function zipTs() {
  const r = await esbuild.build({ entryPoints: [path.join(REPO, 'web/src/app/zip.ts')], bundle: true, format: 'esm', platform: 'neutral', write: false, logLevel: 'silent' });
  return import('data:text/javascript;base64,' + Buffer.from(r.outputFiles[0].text).toString('base64'));
}
const sha256 = b => crypto.createHash('sha256').update(b).digest('hex');
/** Underground 2's page built from this checkout with its launch switch on (src/games/ug2/launch.ts LAUNCHED): a
 *  copy of web/ in dir (beside it the repository's app/, whose registry seed the page takes), its dist folder */
function launchedUg2Page(dir) {
  const web = path.join(dir, 'web');
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(web, { recursive: true });
  fs.copyFileSync(path.join(REPO, 'CHANGELOG.md'), path.join(dir, 'CHANGELOG.md'));
  fs.symlinkSync(path.join(REPO, 'app'), path.join(dir, 'app'));
  for (const f of ['build.mjs', 'index.html']) fs.copyFileSync(path.join(REPO, 'web', f), path.join(web, f));
  fs.cpSync(path.join(REPO, 'web/src'), path.join(web, 'src'), { recursive: true });
  fs.symlinkSync(path.join(REPO, 'web/node_modules'), path.join(web, 'node_modules'));
  const launch = path.join(web, 'src/games/ug2/launch.ts'), was = fs.readFileSync(launch, 'utf8');
  const on = was.replace('export const LAUNCHED = false;', 'export const LAUNCHED = true;');
  if (on === was) throw new Error('src/games/ug2/launch.ts: no "export const LAUNCHED = false;" to turn on');
  fs.writeFileSync(launch, on);
  execFileSync(process.execPath, [path.join(web, 'build.mjs')], { cwd: os.tmpdir(), stdio: 'ignore' });
  return path.join(web, 'dist');
}
/** A phrase made on a game's page (Enable cloud saving) in a browser of its own: the phrase as the page shows it */
async function phraseMadeOn(browser, url) {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 800 } });
  try {
    const b = await ctx.newPage();
    await b.goto(url);
    await b.waitForSelector('#vault:not([hidden])', { timeout: 120000 });
    await b.waitForTimeout(500);
    await b.click('#vault-make');
    await b.waitForFunction(() => { const i = document.querySelector('#vault-input'); return i.readOnly && i.value.length > 20; }, null, { timeout: 60000 });
    return await b.inputValue('#vault-input');
  } finally { await ctx.close(); }
}
/** A phrase typed into a game's page in a fresh browser, Load clicked, and its question (if any) answered Send:
 *  { asked: the question's title and text, or null; msg: the line under the buttons after it; linked } */
async function phraseTypedInto(browser, url, phrase, { answer = true } = {}) {
  const ctx = await browser.newContext({ viewport: { width: 1280, height: 800 } });
  try {
    const b = await ctx.newPage();
    await b.goto(url);
    await b.waitForSelector('#vault:not([hidden])', { timeout: 120000 });
    await b.waitForTimeout(500);
    await b.fill('#vault-input', phrase);
    await b.click('#vault-load');
    const asked = await b.waitForSelector('#confirm[open]', { timeout: 20000 }).then(() => true, () => false);
    let q = null;
    if (asked) {
      q = { title: await b.textContent('#confirm-title'), text: (await b.textContent('#confirm-body'))?.trim(), button: await b.textContent('#confirm-ok') };
      await b.click(answer ? '#confirm-ok' : '#confirm-cancel');
      if (answer) await b.waitForFunction(() => document.querySelector('#vault-input').readOnly, null, { timeout: 60000 }).catch(() => {});
    }
    await b.waitForTimeout(500);
    return { asked: q, msg: (await b.textContent('#vault-msg'))?.trim() ?? '', linked: await b.evaluate(() => document.querySelector('#vault-input').readOnly) };
  } finally { await ctx.close(); }
}
/** a page's file of its save folder (OPFS `save`): its SHA-256, or null */
const opfsHash = (page, save, rel) => page.evaluate(async ([save, rel]) => {
  try {
    let d = await (await navigator.storage.getDirectory()).getDirectoryHandle(save);
    const parts = rel.split('/');
    for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p);
    const f = await (await d.getFileHandle(parts[parts.length - 1])).getFile();
    return [...new Uint8Array(await crypto.subtle.digest('SHA-256', await f.arrayBuffer()))].map(b => b.toString(16).padStart(2, '0')).join('');
  } catch { return null; }
}, [save, rel]);

async function runTwoDomain() {
  const DATA_MW = arg('--data-mw', process.env.NFSMW_DATA || '');
  const MODULE_MW = path.resolve(arg('--module-mw', path.join(REPO, 'out/web/mw')));
  const ORACLE_MW = oracleIn(MODULE_MW, 'nfsmw');
  const BACKUP_U2 = arg('--backup-u2', process.env.NFSU2_BACKUP || ''), SAVES_MW = arg('--saves-mw', process.env.NFSMW_SAVES || '');
  if (!DATA_MW) { console.error('mode two-domain: Most Wanted\'s folder too (--data-mw, or NFSMW_DATA)'); process.exit(2); }
  if (ORACLE_MW && !D3DX) { console.error(`${MODULE_MW}/nfsmw.json: a dev build with the D3DX oracle: give its DLL, --d3dx <d3dx9_26.dll>`); process.exit(2); }
  const dataUg2 = arg('--data', GAMES_OF.ug2.data);
  const [PU, PM] = [P2, P2 + 1];
  const dir = path.join(OUT, 'two-domain');
  fs.rmSync(dir, { recursive: true, force: true });
  fs.mkdirSync(dir, { recursive: true });
  // the box-to-box credential, made up for this run (never written anywhere but these two servers' environment)
  const boxKey = crypto.randomBytes(32).toString('base64url');
  const U2 = `nfsu2.localhost:${P1}`, SITE = `nfsrevived.localhost:${P1}`, u2Origin = `http://${U2}`, siteOrigin = `http://${SITE}`;
  const servers = {};
  let sim = null, browser = null;
  try {
    // each behind the proxy as the boxes run it (the Dockerfile's CMD: --behind-proxy and the name it answers), with
    // the site redirects on as the boxes have them (--site-redirects on)
    const behind = name => ['--behind-proxy', '--host', name, '--site-redirects', 'on'];
    servers.u2 = await startServer(['--http-port', String(PU), ...behind('nfsu2.localhost')],
      { label: 'two-domain-u2', data: dataUg2, state: path.join(dir, 'state-u2'), env: { NFSU2_VAULT_BOX_KEY: boxKey } });
    servers.mw = await startServer(['--game', 'mw', '--http-port', String(PM), ...behind('nfsrevived.localhost'), '--wasm', path.join(MODULE_MW, 'nfsmw.wasm'),
      ...(ORACLE_MW ? ['--dev'] : [])],
      { label: 'two-domain-mw', data: DATA_MW, state: path.join(dir, 'state-mw'),
        env: { NFSU2_VAULT_FORWARD: `http://127.0.0.1:${PU}/vault`, NFSU2_VAULT_BOX_KEY: boxKey } });
    if (ORACLE_MW) report('two-domain: Most Wanted\'s module is a dev build with the D3DX oracle: its server runs with --dev', true, 'PLAN D-09');
    // the edge: each name to its own box (the two tunnels)
    sim = startRouteSim({ port: P1, fakeClientIp: '198.51.100.7',
      route: name => name === 'nfsu2.localhost' ? PU : name === 'nfsrevived.localhost' ? PM : null });
    await sim.ready;
    // 1. COOP and COEP on every path of both names, whatever the answer (redirects, the site's page and 404s too)
    const paths = {
      [U2]: ['/', '/index.html', '/main.js', '/game.json', '/data-manifest', '/players', '/robots.txt', '/nothing', '/vault', '/mw', '/mw/', '/mw/game.json',
        '/mw/?lang=german', '/u2/'],
      [SITE]: ['/', '/robots.txt', '/mw', '/mw/', '/mw/index.html', '/mw/main.js', '/mw/game.json', '/mw/data-manifest', '/mw/players', '/mw/vault',
        '/mw/site/site.css', '/u2', '/u2/', '/u2/index.html?x=1', '/game.json', '/nothing', '/MW/', '/u2x'],
    };
    const missing = [], got = {};
    for (const [host, ps] of Object.entries(paths)) {
      for (const p of ps) {
        const r = await ask(P1, { p, host });
        got[`${host.split(':')[0]}${p}`] = r;
        if (r.headers['cross-origin-opener-policy'] !== 'same-origin' || r.headers['cross-origin-embedder-policy'] !== 'require-corp') missing.push(`${host}${p} (${r.status})`);
      }
    }
    report('two-domain: COOP and COEP on every path of both names', missing.length === 0,
      { missing, statuses: Object.fromEntries(Object.entries(got).map(([k, r]) => [k, r.status])) });
    // 2. each game where it belongs; the redirects to the other game's site, to fixed addresses only
    const want = {
      'nfsu2.localhost/': [200], 'nfsu2.localhost/game.json': [200], 'nfsu2.localhost/robots.txt': [200], 'nfsu2.localhost/u2/': [404],
      'nfsu2.localhost/mw': [308, 'https://nfsrevived.com/mw/'], 'nfsu2.localhost/mw/': [308, 'https://nfsrevived.com/mw/'],
      'nfsu2.localhost/mw/game.json': [308, 'https://nfsrevived.com/mw/'], 'nfsu2.localhost/mw/?lang=german': [308, 'https://nfsrevived.com/mw/'],
      'nfsrevived.localhost/': [200], 'nfsrevived.localhost/robots.txt': [200], 'nfsrevived.localhost/mw': [308, '/mw/'], 'nfsrevived.localhost/mw/': [200],
      'nfsrevived.localhost/mw/game.json': [200], 'nfsrevived.localhost/mw/site/site.css': [200],
      'nfsrevived.localhost/u2': [308, 'https://nfsu2.xyz/'], 'nfsrevived.localhost/u2/': [308, 'https://nfsu2.xyz/'],
      'nfsrevived.localhost/u2/index.html?x=1': [308, 'https://nfsu2.xyz/index.html?x=1'], 'nfsrevived.localhost/game.json': [404],
      'nfsrevived.localhost/MW/': [404], 'nfsrevived.localhost/u2x': [404],
    };
    const off = Object.entries(want).filter(([k, [s, l]]) => got[k]?.status !== s || (l !== undefined && got[k]?.headers.location !== l))
      .map(([k, [s, l]]) => `${k}: ${got[k]?.status} ${got[k]?.headers.location ?? ''}, not ${s} ${l ?? ''}`);
    const site = got['nfsrevived.localhost/'].body, robots = got['nfsrevived.localhost/robots.txt'].body;
    const siteOk = /href="https:\/\/nfsu2\.xyz\/"/.test(site) && /href="\/mw\/"/.test(site) && /<link rel="canonical" href="https:\/\/nfsrevived\.com\/">/.test(site);
    report('two-domain: each game where it belongs, the site\'s page at the root, the redirects to the other game\'s site',
      off.length === 0 && siteOk && /Disallow: \/mw\/data\//.test(robots), off.length ? off : { sitePage: siteOk, robots: robots.split('\n').slice(0, 3) });
    // no open redirect: what names another host is refused or goes to the fixed address
    const probes = [[SITE, '/u2//evil.example/'], [SITE, '/u2/%2f%2fevil.example'], [SITE, '/u2/%5cevil.example'], [SITE, '/u2/@evil.example'],
      [SITE, 'http://evil.example/u2/'], [U2, '/mw//evil.example'], [U2, 'http://evil.example/mw/']];
    const bad = [];
    for (const [host, p] of probes) {
      const r = await ask(P1, { p, host });
      const l = r.headers.location ?? '';
      if (l && !['https://nfsu2.xyz/', 'https://nfsrevived.com/mw/'].some(a => l === a || (l.startsWith(a) && !l.slice(a.length).startsWith('/')))) bad.push(`${host}${p}: ${r.status} ${l}`);
    }
    report('two-domain: no open redirect (only https://nfsu2.xyz/... and https://nfsrevived.com/mw/)', bad.length === 0, bad.length ? bad : `${probes.length} probes`);
    // 3. each game's /lan reaches its own server only
    const count = async (host, p) => JSON.parse((await ask(P1, { p, host })).body || '{}').players;
    const before = [await count(U2, '/players'), await count(SITE, '/mw/players')];
    const u = await lanOpen(P1, U2, '/lan', u2Origin);
    await new Promise(r => setTimeout(r, 300));
    const afterU = [await count(U2, '/players'), await count(SITE, '/mw/players')];
    const m = await lanOpen(P1, SITE, '/mw/lan', siteOrigin);
    await new Promise(r => setTimeout(r, 300));
    const afterM = [await count(U2, '/players'), await count(SITE, '/mw/players')];
    const root = await lanOpen(P1, SITE, '/lan', siteOrigin);
    u.close?.(); m.close?.();
    report('two-domain: each game\'s /lan reaches its own server only', u.status === 101 && m.status === 101 && root.status === 404 &&
      String(before) === '0,0' && String(afterU) === '1,0' && String(afterM) === '1,1',
      { u2: u.status, mw: m.status, siteRootLan: root.status, players: { before, afterU, afterM } });
    // 4. one keyphrase, one vault, the Underground 2 box's: each game's box under its own token from the one phrase,
    // Underground 2's sent to its own /vault, Most Wanted's to /mw/vault on the other name, which goes on through the
    // forwarding; each box readable through either name (one vault); none in Most Wanted's state folder
    const V = { ug2: await vaultOf('ug2'), mw: await vaultOf('mw') };
    const Z = await zipTs();
    const phrase = V.ug2.newPhrase();
    const keys = { ug2: await V.ug2.keysOf(phrase), mw: await V.mw.keysOf(phrase) };
    const page = (origin, token, o = {}) => ({ ...o, headers: { Origin: origin, 'Sec-Fetch-Site': 'same-origin', 'Sec-Fetch-Mode': 'cors',
      Authorization: `Bearer ${token}`, ...(o.headers ?? {}) } });
    // the saves: a real "Download a backup" zip of Underground 2's (--backup-u2) and a folder of Most Wanted's
    // profiles (--saves-mw: NFS Most Wanted/<NAME>/<NAME>); a made-up one of each without them
    const saves = { ug2: [], mw: [] };
    if (BACKUP_U2) for (const e of await Z.unzip(new Uint8Array(fs.readFileSync(BACKUP_U2)))) saves.ug2.push({ path: e.name, data: e.data, mtime: e.mtime ?? 0 });
    else saves.ug2.push({ path: 'NFS Underground 2/TWODOM/TWODOM', data: new Uint8Array(crypto.randomBytes(4096)), mtime: 0 });
    if (SAVES_MW) {
      for (const n of fs.readdirSync(path.join(SAVES_MW, 'NFS Most Wanted'))) {
        const f = path.join(SAVES_MW, 'NFS Most Wanted', n, n);
        if (fs.existsSync(f)) saves.mw.push({ path: `NFS Most Wanted/${n}/${n}`, data: new Uint8Array(fs.readFileSync(f)), mtime: 0 });
      }
    } else saves.mw.push({ path: 'NFS Most Wanted/TWODOM/TWODOM', data: new Uint8Array(crypto.randomBytes(4096)), mtime: 0 });
    // (a profile the phrase keeps: NFS <game>/<NAME>/<NAME>, not a DVD or magazine cover of the same shape)
    const profileOf = g => V[g].kept(saves[g]).find(f => /^NFS (Underground 2|Most Wanted)\/([^/]+)\/\2$/.test(f.path) && !V[g].isCover(f.path));
    const sealed = {};
    for (const g of ['ug2', 'mw']) sealed[g] = await V[g].seal(keys[g].key, await V[g].pack(V[g].kept(saves[g])));
    const putU = await ask(P1, { method: 'PUT', p: '/vault', host: U2, ...page(u2Origin, keys.ug2.token, { headers: { 'If-None-Match': '*' }, body: Buffer.from(sealed.ug2) }) });
    const putM = await ask(P1, { method: 'PUT', p: '/mw/vault', host: SITE, ...page(siteOrigin, keys.mw.token, { headers: { 'If-None-Match': '*' }, body: Buffer.from(sealed.mw) }) });
    const mwAtU2 = await ask(P1, { method: 'HEAD', p: '/vault', host: U2, ...page(u2Origin, keys.mw.token) });
    const u2AtMw = await ask(P1, { method: 'HEAD', p: '/mw/vault', host: SITE, ...page(siteOrigin, keys.ug2.token) });
    const mwFiles = filesUnder(path.join(dir, 'state-mw'));
    report('two-domain: one keyphrase, one vault: Underground 2\'s box at nfsu2/vault, Most Wanted\'s through nfsrevived/mw/vault, each readable from the other name',
      putU.status === 201 && putM.status === 201 && mwAtU2.status === 200 && u2AtMw.status === 200 && keys.ug2.token !== keys.mw.token &&
      filesUnder(path.join(dir, 'state-u2', 'vault')).length > 0 && !mwFiles.some(f => /vault/i.test(f)),
      { putU2: putU.status, putMw: putM.status, mwBoxAtNfsu2: mwAtU2.status, u2BoxAtNfsrevived: u2AtMw.status,
        sealed: { ug2: sealed.ug2.length, mw: sealed.mw.length }, profiles: { ug2: profileOf('ug2')?.path.split('/')[1], mw: saves.mw.length } });
    // 5. in Chrome: the site's page, each game's page with its bar (or none) and its worker; then the one phrase typed
    // into each game's page of a fresh browser restores that game's saves
    browser = await launch();
    const ctx = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const pg = await ctx.newPage();
    const csp = [];
    pg.on('console', msg => { if (/Content Security Policy|Refused to/i.test(msg.text())) csp.push(msg.text().slice(0, 200)); });
    await pg.goto(`${siteOrigin}/`);
    const sitePage = await pg.evaluate(() => ({ links: [...document.querySelectorAll('a.play')].map(a => a.getAttribute('href')), title: document.title,
      styled: getComputedStyle(document.body).backgroundColor, images: [...document.images].map(i => i.naturalWidth) }));
    report('two-domain: the site\'s page in Chrome: both games\' links, its stylesheet and pictures, no CSP refusal',
      JSON.stringify(sitePage.links) === '["https://nfsu2.xyz/","/mw/"]' && sitePage.styled === 'rgb(13, 13, 13)' && sitePage.images.every(w => w > 0) && csp.length === 0,
      { ...sitePage, csp });
    // the site's page and Most Wanted's bar at a phone's width and a computer's: nothing past the window's side, the
    // bar's tabs on the 4 px grid and as tall as a finger needs (44 px or more); a picture of each in the run's folder
    const layout = [];
    for (const w of [320, 390, 1280]) {
      await pg.setViewportSize({ width: w, height: 800 });
      for (const [name, url] of [['site', `${siteOrigin}/`], ['mw-bar', `${siteOrigin}/mw/`]]) {
        await pg.goto(url, { waitUntil: 'domcontentloaded' });
        await pg.waitForTimeout(300);
        const m = await pg.evaluate(() => {
          const tabs = [...document.querySelectorAll('#site-bar .site-tab')].map(a => a.getBoundingClientRect());
          return { over: document.documentElement.scrollWidth - innerWidth,
            tabs: tabs.map(r => [Math.round(r.height), Math.round(r.width)]),
            grid: tabs.every(r => r.height % 4 === 0 && r.top % 4 === 0) };
        });
        await pg.screenshot({ path: path.join(dir, `${name}-${w}.png`), clip: name === 'site' ? undefined : { x: 0, y: 0, width: w, height: Math.min(800, 320) } });
        layout.push({ page: name, width: w, ...m });
      }
    }
    report('two-domain: the site\'s page and Most Wanted\'s bar at 320, 390 and 1280 px: no sideways overflow, tabs on the grid and 44 px or taller',
      layout.every(l => l.over <= 0 && (l.page === 'site' || (l.grid && l.tabs.length === 2 && l.tabs.every(([h]) => h >= 44)))),
      layout.map(l => `${l.page}@${l.width}: over ${l.over}${l.tabs.length ? `, tabs ${l.tabs.map(t => t.join('x')).join(' ')}${l.grid ? '' : ' (off the grid)'}` : ''}`));
    await pg.setViewportSize({ width: 1280, height: 800 });
    const pages = [];
    for (const [origin, p, q] of [[u2Origin, '/', ''], [siteOrigin, '/mw/', ORACLE_MW ? '?env=NFSMW_D3DX=/save/d3dx9_26.dll' : '']]) {
      await pg.goto(`${origin}${p}${q}`);
      await pg.waitForFunction(() => ['ready', 'error'].includes(window.__nfsu2?.phase), null, { timeout: 300000, polling: 250 });
      const t0 = Date.now();
      let regs = [];
      while (Date.now() - t0 < 60000) {
        regs = await pg.evaluate(async () => (await navigator.serviceWorker.getRegistrations()).map(r => new URL(r.scope).pathname));
        if (regs.includes(p)) break;
        await pg.waitForTimeout(500);
      }
      pages.push(await pg.evaluate(([regs]) => ({
        page: location.href, phase: window.__nfsu2.phase, isolated: crossOriginIsolated, workers: regs,
        tabs: [...document.querySelectorAll('#site-bar .site-tab')].map(a => [a.textContent, a.getAttribute('href'), a.getAttribute('aria-current')]),
        picker: !!document.querySelector('#site-bar #lang-pick'),
      }), [regs]));
    }
    const [pu, pm] = pages;
    report('two-domain: Underground 2\'s page as it is (no bar while its launch switch is off), its worker at /',
      pu.isolated && pu.tabs.length === 0 && JSON.stringify(pu.workers) === '["/"]', pu);
    report('two-domain: Most Wanted\'s page with the bar (its tab current, Underground 2\'s to https://nfsu2.xyz/), its worker at /mw/',
      pm.isolated && JSON.stringify(pm.tabs) === JSON.stringify([['Underground 2 Revived', 'https://nfsu2.xyz/', null], ['Most Wanted Revived', null, 'page']]) &&
      pm.picker && JSON.stringify(pm.workers) === '["/mw/"]', pm);
    await ctx.close();
    // a fresh browser: the phrase typed into each game's page loads that game's saves (each page derives its own box
    // from the one phrase, the shared word list and format)
    const fresh = await browser.newContext({ viewport: { width: 1280, height: 800 } });
    const restored = {};
    for (const [g, origin, p, save] of [['ug2', u2Origin, '/', 'save'], ['mw', siteOrigin, '/mw/', 'mw-save']]) {
      const b = await fresh.newPage();
      await b.goto(`${origin}${p}`);
      await b.waitForSelector('#vault:not([hidden])', { timeout: 120000 });
      await b.waitForTimeout(500);
      await b.fill('#vault-input', phrase);
      await b.click('#vault-load');
      await b.waitForSelector('#confirm[open]', { timeout: 30000 });
      await b.click('#confirm-ok');
      const want = profileOf(g);
      const t0 = Date.now();
      let have = null;
      while (Date.now() - t0 < 30000 && have !== sha256(want.data)) { have = await opfsHash(b, save, want.path); await b.waitForTimeout(300); }
      restored[g] = { profile: want.path.split('/')[1], same: have === sha256(want.data), msg: (await b.textContent('#vault-msg'))?.trim() ?? '' };
      await b.close();
    }
    await fresh.close();
    report('two-domain: one keyphrase restores both games\' saves in a fresh browser (Underground 2 at nfsu2, Most Wanted at nfsrevived/mw)',
      restored.ug2.same && restored.mw.same, restored);
    // 5b. the passphrase link (TRACKER M13-10): a phrase made on one game's page, typed into the other's.  The boxes
    // as the server has them, by each game's token through either name (HEAD: the ETag, or 404)
    const boxes = async ph => {
      const out = {};
      for (const g of ['ug2', 'mw']) {
        const r = await ask(P1, { method: 'HEAD', p: '/vault', host: U2, ...page(u2Origin, (await V[g].keysOf(V.ug2.readPhrase(ph).phrase)).token) });
        out[g] = r.status === 200 ? r.headers.etag : r.status;
      }
      return out;
    };
    const question = (here, other) => ({ title: 'Use this passphrase here too?',
      text: `This passphrase keeps your ${other} cloud saves but none of ${here}'s yet. Send the saves here under it too? Each game keeps its own saves.`, button: 'Send' });
    const sent = 'Sent. This browser syncs from now on.';
    // made on Underground 2's page, linked on Most Wanted's
    const pU = await phraseMadeOn(browser, `${u2Origin}/`);
    const b0 = await boxes(pU);
    const toMw = await phraseTypedInto(browser, `${siteOrigin}/mw/`, pU);
    const b1 = await boxes(pU);
    report('two-domain: a phrase made on Underground 2\'s page, typed into Most Wanted\'s: \'Use this passphrase here too?\', Send puts Most Wanted\'s saves under it',
      typeof b0.ug2 === 'string' && b0.mw === 404 && JSON.stringify(toMw.asked) === JSON.stringify(question('Most Wanted', 'Underground 2')) &&
      toMw.msg === sent && toMw.linked && typeof b1.mw === 'string' && b1.ug2 === b0.ug2,
      { phrase: pU.split('-')[0], before: b0, after: b1, page: toMw });
    // made on Most Wanted's page: Underground 2's page as it is (its launch switch off) does not ask
    const pM = await phraseMadeOn(browser, `${siteOrigin}/mw/`);
    const c0 = await boxes(pM);
    const offU2 = await phraseTypedInto(browser, `${u2Origin}/`, pM);
    report('two-domain: Underground 2\'s page with its launch switch off: a phrase of Most Wanted\'s only has no cloud saves there (no question)',
      typeof c0.mw === 'string' && c0.ug2 === 404 && offU2.asked === null && !offU2.linked &&
      offU2.msg.startsWith('No cloud saves for this passphrase.') && JSON.stringify(await boxes(pM)) === JSON.stringify(c0),
      { phrase: pM.split('-')[0], boxes: c0, page: offU2 });
    // then on Underground 2's page with the switch on (built from this checkout; its server started again on it)
    const launched = launchedUg2Page(path.join(dir, 'ug2-launched'));
    await servers.u2.stop();
    servers.u2 = await startServer(['--http-port', String(PU), ...behind('nfsu2.localhost'), '--dist', launched],
      { label: 'two-domain-u2-launched', data: dataUg2, state: path.join(dir, 'state-u2'), env: { NFSU2_VAULT_BOX_KEY: boxKey } });
    const toU2 = await phraseTypedInto(browser, `${u2Origin}/`, pM);
    const c1 = await boxes(pM);
    report('two-domain: a phrase made on Most Wanted\'s page, typed into Underground 2\'s (launch switch on): \'Use this passphrase here too?\', Send puts Underground 2\'s saves under it',
      JSON.stringify(toU2.asked) === JSON.stringify(question('Underground 2', 'Most Wanted')) && toU2.msg === sent && toU2.linked &&
      typeof c1.ug2 === 'string' && c1.mw === c0.mw,
      { before: c0, after: c1, page: toU2 });
    // a phrase of neither: no question, no box
    const none = V.ug2.newPhrase();
    const neither = await phraseTypedInto(browser, `${u2Origin}/`, none);
    report('two-domain: a phrase of neither game: no question, no cloud saves, no box made',
      neither.asked === null && neither.msg.startsWith('No cloud saves for this passphrase.') && JSON.stringify(await boxes(none)) === '{"ug2":404,"mw":404}', neither);
    // 6. the Underground 2 server stopped: /mw/vault 503 from the Most Wanted server (its "unreachable", at once)
    await servers.u2.stop();
    const t1 = Date.now();
    const down = await ask(P1, { p: '/mw/vault', host: SITE, ...page(siteOrigin, keys.mw.token) });
    report('two-domain: the Underground 2 server stopped: /mw/vault 503 from the Most Wanted server', down.status === 503 &&
      down.headers['x-vault-unreachable'] === '1' && Date.now() - t1 < 6000, { status: down.status, unreachable: down.headers['x-vault-unreachable'] ?? null, ms: Date.now() - t1 });
    report('two-domain: Most Wanted\'s state folder holds no vault files', !filesUnder(path.join(dir, 'state-mw')).some(f => /vault/i.test(f)),
      { files: filesUnder(path.join(dir, 'state-mw')).filter(f => !f.startsWith('log/')) });
    report('two-domain: the proxy\'s requests by server', true, { u2: sim.stats.byPort[PU] ?? 0, mw: sim.stats.byPort[PM] ?? 0 });
  } catch (e) {
    report('two-domain: run', false, String(e?.stack ?? e).slice(0, 600));
  } finally {
    await browser?.close();
    await sim?.close();
    for (const s of Object.values(servers)) await s.stop();
  }
}

if (MODE === 'two-domain') await runTwoDomain();
else for (const m of MODES) await run(m);
fs.writeFileSync(path.join(OUT, 'results.json'), JSON.stringify(results, null, 2));
const failed = results.filter(r => !r.ok).length;
console.log(`${results.length - failed}/${results.length} passed; ${OUT}`);
process.exit(failed ? 1 : 0);
