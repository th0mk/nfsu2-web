// The start page's browser check (web/src/app/checks.ts checkBrowser): which browsers get a warning.
//   node --test web/test/checks-browser.test.mjs
// A fake navigator per case: the user agent and the Client Hints brands each browser sends.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const out = await esbuild.build({ entryPoints: [path.join(HERE, '../src/app/checks.ts')], bundle: true, write: false, format: 'esm', platform: 'neutral' });
const C = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));

const WIN = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36';
const IPHONE = 'Mozilla/5.0 (iPhone; CPU iPhone OS 26_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1';
const brands = (...b) => b.map(([brand, version]) => ({ brand, version }));

/** the browser of one case; full: the page may go full screen; home: started from the Home Screen */
function browser({ ua, brands, full = true, home = false }) {
  Object.defineProperty(globalThis, 'navigator', { value: { userAgent: ua, maxTouchPoints: 0, userAgentData: brands && { brands } }, configurable: true, writable: true });
  globalThis.document = { documentElement: full ? { requestFullscreen() {} } : {}, fullscreenEnabled: full };
  globalThis.matchMedia = () => ({ matches: home });
  return C.checkBrowser();
}

test('Chrome and the other Chromium browsers: no warning, each by its own name', () => {
  for (const [b, label] of [
    [brands(['Google Chrome', '155'], ['Chromium', '155'], ['Not)A;Brand', '8']), 'Chrome 155'],
    [brands(['Not A(Brand', '99'], ['Microsoft Edge', '155'], ['Chromium', '155']), 'Edge 155'],
    [brands(['Brave', '155'], ['Chromium', '155'], ['Not_A Brand', '24']), 'Brave 155'],
    [brands(['Chromium', '155'], ['Not/A)Brand', '8']), 'Chromium 155'],               // Vivaldi, plain Chromium
    // Opera's own number, though Chromium's decides
    [brands(['Opera', '125'], ['Not;A=Brand', '99'], ['Chromium', '155']), 'Opera 125'],
  ]) {
    const c = browser({ ua: WIN, brands: b });
    assert.equal(c.state, 'ok', label);
    assert.equal(c.label, label);
  }
});

test('an old Chromium (by Chromium\'s version): the update warning', () => {
  const c = browser({ ua: WIN, brands: brands(['Opera', '110'], ['Chromium', '125'], ['Not.A/Brand', '8']) });
  assert.equal(c.code, 'BROWSER-CHROME-OLD');
  assert.equal(c.label, 'Opera 110');
  assert.equal(browser({ ua: WIN.replace('155', '120') }).code, 'BROWSER-CHROME-OLD');     // no Client Hints
});

test('Safari, and the other browsers on an iPad (Safari\'s engine): no warning', () => {
  const mac = browser({ ua: 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15' });
  assert.equal(mac.state, 'ok');
  assert.equal(mac.label, 'Safari 26');
  const crios = browser({ ua: 'Mozilla/5.0 (iPad; CPU OS 26_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) CriOS/155.0.0.0 Mobile/15E148 Safari/604.1' });
  assert.equal(crios.state, 'ok');
});

test('an iPhone: only the Home Screen tip, none from its icon', () => {
  const c = browser({ ua: IPHONE, full: false });
  assert.equal(c.code, 'BROWSER-IPHONE');
  assert.match(c.detail, /^For full screen on iPhone/);
  assert.equal(browser({ ua: IPHONE, full: false, home: true }).state, 'ok');
});

test('Firefox: tested; another browser: the warning', () => {
  assert.equal(browser({ ua: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:158.0) Gecko/20100101 Firefox/158.0' }).state, 'ok');
  assert.equal(browser({ ua: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:140.0) Gecko/20100101 Firefox/140.0' }).code, 'BROWSER-FIREFOX-OLD');
  assert.equal(browser({ ua: 'SomeBrowser/1.0' }).code, 'BROWSER-OTHER');
});
