// boot.js (web/src/boot.ts): the start page says the game is blocked when main.js never starts, and only then.
//   node --test web/test/boot.test.mjs
// Chrome (or Playwright's Chromium: web/tools/browser.mjs) on web/index.html as this checkout has it, served by the
// test itself (page.route), with boot.js built here as build.mjs builds it and main.js each test's: blocked (the
// request fails), an empty script (an antivirus's stand-in, which loads but never starts), or app/started.ts (the
// real main.js's first step), at once or only after the message.  The page's clock is the test's.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { chromium } from 'playwright';
import { pickBrowser } from '../tools/browser.mjs';
import { bootTexts } from '../tools/i18n-keys.mjs';

const WEB = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const build = (entry, format, define = {}) => esbuild.build({ entryPoints: [path.join(WEB, entry)], bundle: true, write: false, format, define })
  .then(r => r.outputFiles[0].text);
const BOOT = await build('src/boot.ts', 'iife', { BOOT_TEXTS: JSON.stringify(bootTexts()) });
const STARTED = await build('src/app/started.ts', 'esm');
const HTML = fs.readFileSync(path.join(WEB, 'index.html'), 'utf8');
const SITE = 'http://boot.test';

const browser = await chromium.launch({ ...pickBrowser(), headless: true });
test.after(() => browser.close());

/** the page with main.js as `main` ('blocked', 'empty' or 'started'), in `language` when one is given */
async function open(main, language) {
  const p = await browser.newPage();
  await p.clock.install();
  if (language) await p.addInitScript(l => localStorage.setItem('nfsu2-page-language', l), language);
  await p.route(`${SITE}/**`, r => {
    const at = new URL(r.request().url()).pathname;
    const js = body => r.fulfill({ contentType: 'text/javascript', body });
    if (at === '/') return r.fulfill({ contentType: 'text/html', body: HTML });
    if (at === '/boot.js') return js(BOOT);
    if (at === '/main.js') return main === 'blocked' ? r.abort() : js(main === 'started' ? STARTED : '');
    return r.fulfill({ status: 404, body: '' });
  });
  await p.goto(`${SITE}/`);
  return p;
}
const shows = p => p.evaluate(() => ({
  status: document.getElementById('status').textContent, sub: document.getElementById('status-sub').textContent,
  reload: document.getElementById('reload').hidden ? null : document.getElementById('reload').textContent,
}));
const LOADING = { status: 'Loading', sub: 'Loading the game.', reload: null };
const BLOCKED = { status: 'Not loading', sub: 'Your antivirus or network is probably blocking the game. Allow boot.test in it, then reload.', reload: 'Reload' };

test('main.js blocked: the message at once', async () => {
  const p = await open('blocked');
  await p.clock.runFor(100);
  assert.deepEqual(await shows(p), BLOCKED);
  await p.close();
});

test('main.js loads but never starts: the message after 15 s, not before', async () => {
  const p = await open('empty');
  await p.clock.runFor(10_000);
  assert.deepEqual(await shows(p), LOADING);
  await p.clock.runFor(6_000);
  assert.deepEqual(await shows(p), BLOCKED);
  await p.close();
});

test('main.js starts: no message', async () => {
  const p = await open('started');
  await p.clock.runFor(30_000);
  assert.deepEqual(await shows(p), LOADING);
  await p.close();
});

test('main.js starts after the message: the page goes back', async () => {
  const p = await open('empty');
  await p.clock.runFor(16_000);
  assert.deepEqual(await shows(p), BLOCKED);
  await p.addScriptTag({ content: STARTED, type: 'module' });
  assert.deepEqual(await shows(p), LOADING);
  await p.close();
});

test("the page's language", async () => {
  const p = await open('blocked', 'russian');
  await p.clock.runFor(100);
  assert.deepEqual(await shows(p), { status: 'Не загружается',
    sub: 'Скорее всего, игру блокирует ваш антивирус или сеть. Разрешите доступ к boot.test, затем перезагрузите страницу.', reload: 'Перезагрузить' });
  await p.close();
});
