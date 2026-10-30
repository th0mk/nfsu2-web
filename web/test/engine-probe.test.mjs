// The start page's old-engine probe (web/src/app/checks.ts checkEngine, EXNREF_PROBE):
//   node --test web/test/engine-probe.test.mjs
// 1. The probe module is what an engine with WebAssembly's exception references (exnref) takes and one without them
//    refuses: Node's V8 with and without --experimental-wasm-exnref (Node 24's V8 has them behind that flag), and an
//    engine with only the older exceptions (try/catch, which Node 24 has) refuses it too.
// 2. checkEngine's answer: nothing to say where the probe validates; the game module's check with MODULE-OLD-ENGINE
//    where it does not, or where validate throws.
// 3. The page in Chrome (web/tools/browser.mjs) on this checkout's web/dist (node web/build.mjs) from the server
//    (web/tools/server-cmd.mjs), as a browser from before exnref (WebAssembly.validate refuses the probe, every compile
//    fails as such an engine's would): the game module's check is MODULE-OLD-ENGINE, never MODULE-LOAD, the page never
//    asks for the module, and Retry changes nothing.  Skipped without a built page or a server.
// Port: NFSU2_TEST_ENGINE_PORT (8539).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const out = await esbuild.build({ entryPoints: [path.join(HERE, '../src/app/checks.ts')], bundle: true, write: false, format: 'esm', platform: 'neutral' });
const C = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));

/** WebAssembly.validate of these bytes in a Node with these flags: true, false, or null (Node does not take a flag) */
function validateIn(bytes, flags) {
  const r = spawnSync(process.execPath, [...flags, '-e', `process.stdout.write(String(WebAssembly.validate(new Uint8Array(${JSON.stringify([...bytes])}))))`], { encoding: 'utf8' });
  return r.status === 0 ? r.stdout === 'true' : null;
}
/** a module with the older exceptions only: one function, try (empty) catch_all end */
const LEGACY = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03, 0x02, 0x01, 0x00,
  0x0a, 0x08, 0x01, 0x06, 0x00, 0x06, 0x40, 0x19, 0x0b, 0x0b];

test('the probe: an engine with exnref takes it, one without refuses it', t => {
  const on = validateIn(C.EXNREF_PROBE, ['--experimental-wasm-exnref']);
  if (on === null) {
    // a Node whose V8 has exnref without a flag (and no longer takes it): the probe validates as it is
    assert.equal(validateIn(C.EXNREF_PROBE, []), true);
    t.diagnostic('this Node has exnref without a flag: only the "with" case');
    return;
  }
  assert.equal(on, true, 'with exnref');
  const off = validateIn(C.EXNREF_PROBE, ['--no-experimental-wasm-exnref']);
  assert.equal(off, false, 'without exnref');
  // the engines it is for still have the older exceptions: they do not pass it for those
  if (validateIn(LEGACY, ['--no-experimental-wasm-exnref']) === true) t.diagnostic('the older exceptions alone: refused');
  // and one byte off is not a module at all (the probe's lengths are right)
  assert.equal(validateIn(C.EXNREF_PROBE.slice(0, -1), ['--experimental-wasm-exnref']), false);
});

test('checkEngine: null with exnref; the module check with its code without', () => {
  assert.equal(C.checkEngine(() => true), null);
  for (const v of [() => false, () => { throw new Error('no WebAssembly'); }]) {
    const c = C.checkEngine(v);
    assert.equal(c.id, 'module');
    assert.equal(c.state, 'fail');
    assert.equal(c.code, 'MODULE-OLD-ENGINE');
    assert.equal(c.tl, 'Game module');
    assert.match(c.detail, /too old to run the game .*Update the browser\.$/);
  }
  // the probe goes to validate as it is
  let seen;
  C.checkEngine(b => { seen = b; return true; });
  assert.equal(seen, C.EXNREF_PROBE);
});

const PORT = +(process.env.NFSU2_TEST_ENGINE_PORT || 8539);
const { SERVER_BIN, serverCommand } = await import('../tools/server-cmd.mjs');
const SKIP = !fs.existsSync(path.join(HERE, '../dist/main.js')) ? 'no page built (node web/build.mjs)'
  : process.platform !== 'win32' && !fs.existsSync(SERVER_BIN) ? 'no server (web/server-rs/build.sh)' : false;

/** In the page, before its scripts: an engine from before exnref, as the page sees one */
function oldEngine() {
  const validate = WebAssembly.validate;
  const refuse = () => Promise.reject(new WebAssembly.CompileError('invalid value type \'exn\''));
  WebAssembly.validate = function (b) {
    const u = b instanceof Uint8Array ? b : new Uint8Array(b instanceof ArrayBuffer ? b : b.buffer);
    // 0x69: exnref, as a block type right after the code section's block opcode (EXNREF_PROBE)
    for (let i = 0; i + 1 < u.length; i++) if (u[i] === 0x02 && u[i + 1] === 0x69) return false;
    return validate.call(WebAssembly, b);
  };
  WebAssembly.compile = refuse;
  WebAssembly.compileStreaming = refuse;
  WebAssembly.instantiateStreaming = refuse;
}

test('the page in a browser from before exnref: MODULE-OLD-ENGINE, no module download, no retries', { skip: SKIP, timeout: 120_000 }, async () => {
  const { chromium } = await import('playwright');
  const { pickBrowser } = await import('../tools/browser.mjs');
  const tmp = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'nfsu2-engine-test-'));
  const data = path.join(tmp, 'game');
  fs.mkdirSync(data);
  fs.writeFileSync(path.join(data, 'SPEED2.EXE'), 'MZ');
  const srv = spawn(...serverCommand(['--data', data, '--http-port', String(PORT), '--state', path.join(tmp, 'state')]), { stdio: ['ignore', 'pipe', 'pipe'] });
  let browser;
  try {
    await new Promise((res, rej) => {
      let o = '';
      const t = setTimeout(() => rej(new Error('server did not start: ' + o)), 15000);
      srv.stdout.on('data', d => { o += d; if (o.includes('  access    :')) { clearTimeout(t); res(); } });
      srv.on('exit', c => { clearTimeout(t); rej(new Error('server exited ' + c + ' ' + o)); });
    });
    browser = await chromium.launch({ ...pickBrowser(), headless: true });
    const page = await browser.newPage();
    const asked = [], errors = [];
    page.on('request', r => { if (/\.wasm(\?|$)|\/module(\?|$)/.test(r.url())) asked.push(r.url()); });
    page.on('pageerror', e => errors.push(String(e)));
    await page.addInitScript(oldEngine);
    await page.goto(`http://localhost:${PORT}/`);
    const module = () => page.evaluate(() => window.__nfsu2?.checks().find(c => c.id === 'module') ?? null);
    await page.waitForFunction(() => window.__nfsu2?.checks().find(c => c.id === 'module')?.state === 'fail', null, { timeout: 60_000, polling: 100 });
    let m = await module();
    assert.equal(m.code, 'MODULE-OLD-ENGINE', JSON.stringify(m));
    // Retry (the page's own call for it), then the back-off's first two steps: still the same check, no download
    await page.evaluate(() => window.__nfsu2.retryModule());
    await page.waitForTimeout(6_500);
    m = await module();
    assert.equal(m.code, 'MODULE-OLD-ENGINE', JSON.stringify(m));
    assert.deepEqual(asked, [], 'the module was asked for');
    const shown = await page.evaluate(() => [...document.querySelectorAll('#problems li')].map(li => li.textContent).join('\n'));
    assert.match(shown, /MODULE-OLD-ENGINE/);
    assert.doesNotMatch(shown, /MODULE-LOAD/);
    const log = await page.evaluate(() => window.__nfsu2.log.text());
    assert.equal(log.split('\n').filter(l => l.includes('MODULE-OLD-ENGINE')).length, 1, 'one log line');
    assert.deepEqual(errors, []);
  } finally {
    await browser?.close();
    const gone = srv.exitCode !== null || srv.signalCode !== null ? null : new Promise(r => srv.once('exit', r));
    srv.kill(); await gone;
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});
