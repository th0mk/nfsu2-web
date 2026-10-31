// The start page 320 px wide, in each of its languages: no sideways scroll.
//   node --test web/test/page-narrow.test.mjs
// Chrome (or Playwright's Chromium: web/tools/browser.mjs) on this checkout's page (web/dist: node web/build.mjs; a
// web/dist older than one of the page's sources fails at once, never measures the older page) from the server
// (web/tools/server-cmd.mjs), on the game folder (NFSU2_DATA) or, without one, a made-up one: the page's layout needs
// no game.  As a phone (touch: the touch controls' settings) and as a window with a mouse (a desktop zoomed in: the
// keys), each with cloud saves turned on first (their buttons).  Then in every language (i18n.ts LANGUAGES), with every
// <details> open, on each Controls tab: the page as it is, and again with every button and line the page hides until it
// needs one shown (Retry, the bug report's line, a cloud conflict's buttons, …).  The page is never wider than the
// screen, and no box or text reaches past the screen's edges or into its panel's padding.
// The widths are those of the fonts the page names (style.css --font: Helvetica Neue on a Mac, Arial on Windows): a
// machine with none of them draws its text in a wider fallback (Linux without fonts-liberation, Arial's widths: DejaVu
// Sans), and the test fails at once saying so, as it cannot measure the page players see.
// Port: NFSU2_TEST_PAGE_PORT (8534).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { chromium } from 'playwright';
import { pickBrowser } from '../tools/browser.mjs';
import { serverCommand, SERVER_BIN } from '../tools/server-cmd.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PORT = +(process.env.NFSU2_TEST_PAGE_PORT || 8534);
const WIDTH = 320;
const out = await esbuild.build({ entryPoints: [path.join(HERE, '../src/app/i18n.ts')], bundle: true, write: false, format: 'esm', platform: 'neutral' });
const { LANGUAGES } = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));
const SKIP = !fs.existsSync(path.join(HERE, '../dist/main.js')) ? 'no page built (node web/build.mjs)'
  : process.platform !== 'win32' && !fs.existsSync(SERVER_BIN) ? 'no server (web/server-rs/build.sh)' : false;

/** The page's source (what web/build.mjs reads) newer than the built page in web/dist, or '': the test measures the
 *  page as web/dist has it, so an older build would measure the old stylesheet and text, not this checkout's. */
function newerSource() {
  const web = path.join(HERE, '..');
  const built = Math.min(...['main.js', 'main.css', 'index.html'].map(f => fs.statSync(path.join(web, 'dist', f), { throwIfNoEntry: false }))
    .filter(Boolean).map(s => s.mtimeMs));
  const src = ['index.html', 'build.mjs', '../app/registry-defaults.txt', ...fs.readdirSync(path.join(web, 'src'), { recursive: true }).map(f => path.join('src', String(f)))];
  let newest = '', at = built;
  for (const f of src) {
    const s = fs.statSync(path.join(web, f), { throwIfNoEntry: false });
    if (s?.isFile() && s.mtimeMs > at) { at = s.mtimeMs; newest = path.join('web', f); }
  }
  return newest;
}

async function server() {
  const tmp = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'nfsu2-page-test-'));
  let data = process.env.NFSU2_DATA || path.join(os.homedir(), 'projects/nsfu2/Need for Speed - Underground 2');
  if (!fs.existsSync(path.join(data, 'SPEED2.EXE'))) {
    data = path.join(tmp, 'game');
    fs.mkdirSync(data);
    fs.writeFileSync(path.join(data, 'SPEED2.EXE'), 'MZ');
  }
  const srv = spawn(...serverCommand(['--data', data, '--http-port', String(PORT), '--state', path.join(tmp, 'state')]), { stdio: ['ignore', 'pipe', 'pipe'] });
  await new Promise((res, rej) => {
    let o = '';
    const t = setTimeout(() => rej(new Error('server did not start: ' + o)), 15000);
    srv.stdout.on('data', d => { o += d; if (o.includes('  access    :')) { clearTimeout(t); res(); } });
    srv.on('exit', c => { clearTimeout(t); rej(new Error('server exited ' + c + ' ' + o)); });
  });
  return {
    stop: async () => {
      const gone = srv.exitCode !== null || srv.signalCode !== null ? null : new Promise(r => srv.once('exit', r));
      srv.kill(); await gone;
      fs.rmSync(tmp, { recursive: true, force: true });
    },
  };
}

/** In the page: how much wider than the screen it is, and the boxes and text that reach past the screen's edges or
 *  into their panel's padding (unless a box that clips them keeps them in), in the window's coordinates */
function measure() {
  const vw = document.documentElement.clientWidth, past = [], intoPadding = [];
  const name = el => {
    const set = el.closest('.set[data-key]'), sec = el.closest('section, .top');
    return `${sec ? (sec.id || sec.className.split(' ')[0]) + ' ' : ''}${set && set !== el ? `[${set.dataset.key}] ` : ''}${el.tagName.toLowerCase()}` +
      `${el.id ? '#' + el.id : ''}${typeof el.className === 'string' && el.className ? '.' + el.className.trim().split(/\s+/).join('.') : ''}`;
  };
  /** a box from el up to stop that clips what reaches past right */
  const clipped = (el, stop, right) => {
    for (let a = el; a && a !== stop; a = a.parentElement)
      if (getComputedStyle(a).overflowX !== 'visible' && a.getBoundingClientRect().right <= right + 0.5) return true;
    return false;
  };
  const check = (el, b, what) => {
    if (!b.width && !b.height) return;
    if ((b.right > vw + 0.5 || b.left < -0.5) && !clipped(el, null, vw)) past.push(`${what} ${b.left.toFixed(1)}..${b.right.toFixed(1)}`);
    const panel = el.closest('.panel, .top');
    if (!panel || panel === el || el.closest('.top > .bar')) return;   // the loading bar: a line along the header's edge
    const ps = getComputedStyle(panel), pb = panel.getBoundingClientRect();
    const left = pb.left + parseFloat(ps.paddingLeft) + parseFloat(ps.borderLeftWidth);
    const right = pb.right - parseFloat(ps.paddingRight) - parseFloat(ps.borderRightWidth);
    if ((b.right > right + 0.5 || b.left < left - 0.5) && !clipped(el, panel, right))
      intoPadding.push(`${what} ${b.left.toFixed(1)}..${b.right.toFixed(1)}, its panel's ${left.toFixed(1)}..${right.toFixed(1)}`);
  };
  for (const el of document.querySelectorAll('.app *')) {
    if (el.closest('svg') && el.tagName.toLowerCase() !== 'svg') continue;
    if (el.getClientRects().length) check(el, el.getBoundingClientRect(), name(el));
  }
  // text: a word wider than its box reaches out of the box, which itself stays in
  const walk = document.createTreeWalker(document.querySelector('.app'), NodeFilter.SHOW_TEXT), r = document.createRange();
  for (let n = walk.nextNode(); n; n = walk.nextNode()) {
    if (!n.data.trim() || !n.parentElement.getClientRects().length) continue;
    r.selectNodeContents(n);
    check(n.parentElement, r.getBoundingClientRect(), `"${n.data.trim().slice(0, 24)}" in ${name(n.parentElement)}`);
  }
  return { wider: document.documentElement.scrollWidth - vw, past, intoPadding };
}

/** In the page: the families its font (--font) names, and those the browser has (one it lacks draws a line of text as
 *  wide as the generic family after it) */
function fontsHad() {
  const named = getComputedStyle(document.documentElement).getPropertyValue('--font').split(',').map(f => f.trim())
    .filter(f => f && !/^(serif|sans-serif|monospace|cursive|fantasy|system-ui|ui-[a-z-]+)$/.test(f));
  const g = document.createElement('canvas').getContext('2d');
  const w = font => { g.font = `16px ${font}`; return g.measureText('The quick brown fox jumps over the lazy dog 0123').width; };
  return { named, had: named.filter(f => ['monospace', 'serif'].some(b => w(`${f}, ${b}`) !== w(b))) };
}

/** One browser's pass over every language (touch: as a phone): what is too wide, by look; how many looks; the page's
 *  phase; its errors */
async function look(browser, kind, touch) {
  const ctx = await browser.newContext({ viewport: { width: WIDTH, height: 640 }, deviceScaleFactor: 2, hasTouch: touch, isMobile: touch });
  try {
    const page = await ctx.newPage();
    const errors = [], found = {};
    let looks = 0;
    page.on('pageerror', e => errors.push(String(e)));
    await page.goto(`http://localhost:${PORT}/`);
    const phase = await page.waitForFunction(() => ['ready', 'error'].includes(window.__nfsu2?.phase) && window.__nfsu2.phase, null, { timeout: 120_000, polling: 250 }).then(h => h.jsonValue());
    const fonts = await page.evaluate(fontsHad);
    if (!fonts.had.length) throw new Error(`none of the page's fonts (${fonts.named.join(', ')}) is on this machine: the browser ` +
      'draws its text in a fallback with other widths, so what it measures is not the page players see' +
      (process.platform === 'linux' ? ' (install fonts-liberation: Arial\'s widths)' : ''));
    // the language is a setting: the tab owns the saves and has read its settings
    await page.waitForFunction(() => !!document.querySelector('#settings .set') && !document.querySelector('#settings').hasAttribute('data-readonly'), null, { timeout: 30_000 });
    await page.click('#vault-make');
    await page.waitForFunction(() => !document.querySelector('#vault-on').hidden, null, { timeout: 30_000 });
    const frames = () => page.evaluate(() => document.fonts.ready.then(() => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)))));
    for (const lang of LANGUAGES) {
      if (await page.evaluate(() => document.querySelector('#lang-pick').value) !== lang) {
        const was = await page.evaluate(() => document.documentElement.lang);
        await page.selectOption('#lang-pick', lang);
        await page.waitForFunction(w => document.documentElement.lang !== w, was, { timeout: 15_000 });
      }
      for (const shown of ['as it is', 'everything shown']) {
        await page.evaluate(all => {
          for (const d of document.querySelectorAll('.app details')) d.open = true;
          if (all) window.__shown = [...document.querySelectorAll('.app [hidden]:not(.tab-panel)')].map(el => { el.hidden = false; return el; });
        }, shown === 'everything shown');
        for (const tab of ['keys', 'pad', 'site']) {
          await page.click(`.tab[data-tab="${tab}"]`);
          await frames();
          const m = await page.evaluate(measure);
          looks++;
          if (m.wider > 0 || m.past.length || m.intoPadding.length) found[`${kind}, ${lang}, ${shown}, ${tab} tab`] = m;
        }
        await page.click('.tab[data-tab="keys"]');
        // hidden again, as the page had them, before the next language
        await page.evaluate(() => { for (const el of window.__shown ?? []) el.hidden = true; window.__shown = []; });
      }
    }
    return { found, looks, phase, errors };
  } finally {
    await ctx.close();
  }
}

test(`the start page at ${WIDTH} px: no sideways scroll in any language`, { skip: SKIP, timeout: 600_000 }, async t => {
  const stale = newerSource();
  assert.equal(stale, '', `web/dist is older than ${stale}: build the page (node web/build.mjs), then run this again`);
  const srv = await server();
  let browser;
  try {
    try { browser = await chromium.launch({ ...pickBrowser(), headless: true, args: ['--enable-unsafe-webgpu'] }); }
    catch (e) { t.skip(`no browser: ${String(e.message).split('\n')[0]}`); return; }
    const found = {}, errors = [];
    let looks = 0;
    for (const [kind, touch] of [['a phone', true], ['a mouse', false]]) {
      const r = await look(browser, kind, touch);
      Object.assign(found, r.found);
      errors.push(...r.errors);
      looks += r.looks;
      console.log(`${kind}: ${LANGUAGES.length} languages at ${WIDTH} px (page ${r.phase}, cloud saves on), ${r.looks} looks: ` +
        (Object.keys(r.found).length ? `${Object.keys(r.found).length} too wide` : 'none wider than the screen, nothing past an edge'));
    }
    for (const [k, m] of Object.entries(found))
      console.log(`  ${k}: ${m.wider > 0 ? `${m.wider} px wider than the screen; ` : ''}${[...m.past.map(p => 'past the screen: ' + p), ...m.intoPadding.map(p => 'into its panel\'s padding: ' + p)].slice(0, 6).join('; ')}`);
    assert.deepEqual(errors, [], 'no page errors');
    const bad = Object.keys(found);
    assert.equal(bad.length, 0, `${bad.length} of ${looks} looks too wide, e.g. ${bad.slice(0, 4).join('; ')}`);
  } finally {
    await browser?.close();
    await srv.stop();
  }
});
