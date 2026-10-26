// End to end in Chrome over TLS, as a friend would reach a VPS: the start
// page, the security of the page (secure context, cross-origin isolation, no
// CSP violations or policy warnings, nothing machine-specific on the page),
// Play, and the game up to its title screen or (--menu, with the real input
// and renderer of the integrated app) the main menu, with a screenshot.
//
//   node web/tools/deploy-e2e.mjs --out <dir> [--mode tls|proxy|both|docker] [--menu] [--headed]
//        [--data "<game folder>"] [--port 8527] [--http-port 8627] [--server <binary>]
//        [--image <name>] [--module <dir>]
//        [--game ug2|mw] (the game's title and menu screens; Most Wanted has none yet) [--help] (this text and those)
// --server: another server than web/tools/server-cmd.mjs's, such as the nfsu2-server of an unpacked bundle
// (web/deploy/pack.sh), with the page and the module of the folder it is in (NFSU2_REPO).
// --proxy-cmd "<command>": in mode proxy, this command is the proxy instead of the Cloudflare stand-in
// (a real Caddy with the example Caddyfile's directives, say); it must serve the test certificate on
// 127.0.0.1:<port> and forward to 127.0.0.1:<http-port>.  It is stopped at the end.
//
// mode tls:    the server with --tls-cert <test certificate>   (Let's Encrypt / Origin CA on a VPS)
// mode proxy:  the server with --behind-proxy, and web/test/cf-sim.mjs in front (Cloudflare)
// mode docker: the Docker stack, web/deploy/compose.yml (the server's image and Caddy), with the test
//              certificate in Caddy instead of Let's Encrypt, its HTTPS on 127.0.0.1:<port> and the server on
//              127.0.0.1:<http-port> of the Docker host (with Colima or Docker Desktop: their Linux VM, which
//              forwards <port> to this Mac); not in "both".  --image <name> (default nfsu2-web:dev, from
//              docker build -f web/server-rs/Dockerfile -t nfsu2-web:dev .), --module <dir> (nfsu2.wasm + nfsu2.json; default out/web).
//              Needs docker compose; the stack is removed at the end.
// Chrome reaches https://nfsu2.test:<port>/ (mapped to 127.0.0.1) and accepts
// the test certificate by its key (--ignore-certificate-errors-spki-list), so
// the page is a real secure origin.  Nothing listens beyond 127.0.0.1 except
// the server's own TLS port in mode tls.  Exit 0 = every check passed.
import { chromium } from 'playwright';
import fs from 'node:fs';
import os from 'node:os';
import net from 'node:net';
import path from 'node:path';
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { makeTestCerts, startCfSim } from '../test/cf-sim.mjs';
import { serverCommand } from './server-cmd.mjs';
import { gameOf, usage, nothingFor } from './rig-game.mjs';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
// each game's screens, as the front end's log names their packages: the title Play reaches, the main menu of --menu
const SCREENS_OF = { ug2: { title: 'UG_LS_Splash.fng', menu: 'GarageMain.fng' } };
const NONE = 'none yet: no Most Wanted title or menu screens (M11-16)';
const GAME = gameOf(process.argv.slice(2)), SCREENS = SCREENS_OF[GAME];
if (process.argv.includes('--help')) usage(import.meta.url, GAME, 'Screens', SCREENS ? [`title: ${SCREENS.title}`, `--menu: ${SCREENS.menu}`] : [], NONE);
if (!SCREENS) nothingFor(GAME, NONE);
const OUT = path.resolve(arg('--out', 'out/web/deploy-e2e'));
const MODES = arg('--mode', 'both') === 'both' ? ['tls', 'proxy'] : [arg('--mode')];
const MENU = process.argv.includes('--menu');
const DATA = arg('--data', process.env.NFSU2_DATA || path.join(os.homedir(), 'projects/nsfu2/Need for Speed - Underground 2'));
const P1 = +arg('--port', 8527), P2 = +arg('--http-port', 8627);
const SERVER = arg('--server', '') && path.resolve(arg('--server'));
const PROXY_CMD = arg('--proxy-cmd', '');
const IMAGE = arg('--image', 'nfsu2-web:dev');
const MODULE = path.resolve(arg('--module', path.join(REPO, 'out/web')));
fs.mkdirSync(OUT, { recursive: true });
const certs = makeTestCerts(path.join(OUT, 'certs'));
const results = [];
const report = (step, ok, evidence) => {
  results.push({ step, ok, evidence });
  console.log(`${ok ? 'PASS' : 'FAIL'} ${step}: ${typeof evidence === 'string' ? evidence : JSON.stringify(evidence)}`);
};
const machine = [os.hostname(), os.hostname().replace(/\.local$/, ''), os.homedir(),
  ...Object.values(os.networkInterfaces()).flat().filter(a => a && !a.internal).map(a => a.address)];

async function startServer(args) {
  const state = path.join(OUT, `state-${args.includes('--behind-proxy') ? 'proxy' : 'tls'}`);
  const c = spawn(...serverCommand(['--data', DATA, '--state', state, '--access-log', ...args], SERVER || undefined),
    { stdio: ['ignore', 'pipe', 'pipe'], env: SERVER ? { ...process.env, NFSU2_REPO: path.dirname(SERVER) } : process.env });
  const log = fs.createWriteStream(path.join(OUT, `server-${args.includes('--behind-proxy') ? 'proxy' : 'tls'}.log`));
  c.stdout.pipe(log); c.stderr.pipe(log);
  await new Promise((res, rej) => {
    let out = '';
    const t = setTimeout(() => rej(new Error('server did not start')), 60000);
    c.stdout.on('data', d => { out += d; if (out.includes('  access    :')) { clearTimeout(t); res(); } });
    c.on('exit', code => { clearTimeout(t); rej(new Error(`server exited (${code}): ${out}`)); });
  });
  return { stop: () => new Promise(r => { c.once('exit', r); c.kill('SIGTERM'); }) };
}

// the shipped stack, but Caddy serves the test certificate on 127.0.0.1:<port> only, without its http -> https
// port, with a Caddyfile of its own (the proxy and header -Server; compose.yml's other directives are not in it)
async function startDocker() {
  const override = path.join(OUT, 'compose.e2e.yml');
  fs.writeFileSync(override, `services:
  caddy:
    configs: !override [{ source: caddyfile-e2e, target: /etc/caddy/Caddyfile }]
    volumes: ["${certs.dir}:/certs:ro"]
configs:
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
  const compose = (...a) => execFileSync('docker', ['compose', '-p', 'nfsu2-e2e', '-f', path.join(REPO, 'web/deploy/compose.yml'), '-f', override, ...a],
    { env, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  // --wait: until the image's health check sees the module served
  compose('up', '-d', '--wait', '--wait-timeout', '120');
  return { stop: async () => {
    fs.writeFileSync(path.join(OUT, 'server-docker.log'), compose('logs', '--no-color', 'server', 'caddy'));
    compose('down', '--volumes');
  } };
}

async function run(mode) {
  const server = mode === 'tls'
    ? await startServer(['--port', String(P1), '--http-port', '0', '--tls-cert', certs.cert, '--tls-key', certs.key])
    : mode === 'docker' ? await startDocker()
    : await startServer(['--behind-proxy', '--http-port', String(P2), '--host', 'nfsu2.test']);
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
  const browser = await chromium.launch({ channel: 'chrome', headless: !process.argv.includes('--headed'), args: [
    '--enable-unsafe-webgpu', `--host-resolver-rules=MAP nfsu2.test 127.0.0.1`,
    `--ignore-certificate-errors-spki-list=${certs.spki.join(',')}`] });
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
  const t0 = Date.now();
  try {
    // a production server: the page takes no settings from its address (the audit's PAGE-1), so the test's own
    // (no movies, the front-end log the checks below read, free roam answered: the first Play would ask) go into
    // this fresh browser's settings.ini, as a player's would
    const resp = await page.goto(`${origin}/`);
    const h = resp.headers();
    report(`${mode}: document headers`, !!h['content-security-policy'] && h['cross-origin-embedder-policy'] === 'require-corp' &&
      h['cross-origin-opener-policy'] === 'same-origin' && h['strict-transport-security'] === 'max-age=31536000' && /no-store/.test(h['cdn-cache-control'] ?? ''),
      { hsts: h['strict-transport-security'], coep: h['cross-origin-embedder-policy'], cache: h['cache-control'], server: h.server ?? null });
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
    await page.evaluate(async () => {
      const d = await (await navigator.storage.getDirectory()).getDirectoryHandle('save', { create: true });
      const w = await (await d.getFileHandle('settings.ini', { create: true })).createWritable();
      await w.write('# NFSU2 settings, read at every launch.\nmovies = off\nNFSU2_FNG_LOG = 1\nroam = off\n'); await w.close();
    });
    // Play
    await page.evaluate(() => window.__nfsu2.log.clear());
    await page.click('#play');
    const tp = Date.now();
    await page.waitForFunction(t => window.__nfsu2.log.tail(400).some(l => l.includes(`top package ${t}`)), SCREENS.title, { timeout: 300000, polling: 200 });
    report(`${mode}: Play -> the game's title screen`, true, { ms: Date.now() - tp });
    if (MENU) {
      // Enter through the title and the profile prompt to the main menu (GarageMain), with real key events
      const tm = Date.now();
      let reached = false;
      while (Date.now() - tm < 120000 && !reached) {
        await page.keyboard.press('Enter');
        await page.waitForTimeout(1500);
        reached = await page.evaluate(m => window.__nfsu2.log.tail(600).some(l => l.includes(`top package ${m}`)), SCREENS.menu);
      }
      await page.waitForTimeout(2500);
      await page.screenshot({ path: path.join(OUT, `${mode}-menu.png`) });
      const gpu = await page.evaluate(() => window.__nfsu2.log.tail(2000).filter(l => /renderer attached|gpu|WebGPU/i.test(l)).slice(-4));
      report(`${mode}: main menu with the renderer`, reached, { ms: Date.now() - tm, screenshot: `${mode}-menu.png`, renderer: gpu });
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
    await browser.close();
    if (sim) await sim.close();
    await server.stop();
  }
}

for (const m of MODES) await run(m);
fs.writeFileSync(path.join(OUT, 'results.json'), JSON.stringify(results, null, 2));
const failed = results.filter(r => !r.ok).length;
console.log(`${results.length - failed}/${results.length} passed; ${OUT}`);
process.exit(failed ? 1 : 0);
