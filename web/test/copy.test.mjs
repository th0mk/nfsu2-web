// The copy buttons' toast (web/src/app/copy.ts), in a real browser (web/tools/browser.mjs):
//   node --test web/test/copy.test.mjs
// - copyToast: over the button's middle, 8 px above it; under it where there is no room above; inside the window at
//   its edges; over a modal dialog (the top layer); one at a time a button; gone after its time
// - copyText: the Clipboard API, else the old way; false when neither copies
// The module is bundled from web/src with esbuild into a blank page.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { chromium } from 'playwright';
import { pickBrowser } from '../tools/browser.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const js = (await esbuild.build({
  stdin: { contents: `export * from './web/src/app/copy.ts';`, resolveDir: ROOT, loader: 'ts' },
  bundle: true, format: 'iife', globalName: 'C', write: false, platform: 'browser',
})).outputFiles[0].text;
const css = (await import('node:fs')).readFileSync(path.join(ROOT, 'web/src/app/style.css'), 'utf8');

const browser = await chromium.launch({ ...pickBrowser(), headless: true });
test.after(() => browser.close());

async function page() {
  const p = await browser.newPage({ viewport: { width: 800, height: 600 } });
  await p.setContent(`<style>${css}</style><button id="mid" style="position:fixed;left:380px;top:300px;width:40px;height:40px"></button>
    <button id="top" style="position:fixed;left:380px;top:4px;width:40px;height:40px"></button>
    <button id="edge" style="position:fixed;left:772px;top:300px;width:24px;height:40px"></button>
    <dialog id="dlg"><button id="inner" style="width:40px;height:40px">x</button></dialog>`);
  await p.addScriptTag({ content: js });
  return p;
}
/** a toast at button id, and the toasts there are once it has settled (its fade-in moves it 4 px) */
const toast = (p, id, text, ms) => p.evaluate(async ([id, text, ms]) => {
  /** @type {any} */ (window).C.copyToast(document.getElementById(id), text, ms);
  await new Promise(r => setTimeout(r, 250));
  const els = [...document.querySelectorAll('.copy-toast')];
  return els.map(e => { const r = e.getBoundingClientRect(); return { text: e.textContent, x: r.x, y: r.y, w: r.width, h: r.height, below: e.classList.contains('below'), top: e.matches(':popover-open') }; });
}, [id, text, ms]);

test('a toast at the button: over it, under it with no room above, inside the window, one a button, then gone', async () => {
  const p = await page();
  const [t] = await toast(p, 'mid', 'Copied', 1000);
  assert.equal(t.text, 'Copied');
  assert.ok(t.top, 'in the top layer');
  assert.equal(t.y + t.h, 300 - 8, '8 px over the button');
  assert.ok(Math.abs(t.x + t.w / 2 - 400) <= 1, 'over its middle');
  assert.equal(t.below, false);
  const again = await toast(p, 'mid', 'Copied again', 1000);
  assert.deepEqual(again.map(x => x.text), ['Copied again'], 'the new one in the old one\'s place');
  const top = (await toast(p, 'top', 'At the top', 300)).find(x => x.text === 'At the top');
  assert.ok(top && top.below && top.y === 4 + 40 + 8, 'no room above: under it');
  const edge = (await toast(p, 'edge', 'Copied a long name here', 300)).find(x => x.text.startsWith('Copied a long'));
  assert.ok(edge.x + edge.w <= 800 - 8 && edge.x >= 8, 'inside the window');
  await p.waitForTimeout(1300);
  assert.equal(await p.$$eval('.copy-toast', es => es.length), 0, 'gone after its time');
  await p.close();
});

test('a button in a modal dialog: the toast over the dialog', async () => {
  const p = await page();
  await p.evaluate(() => /** @type {HTMLDialogElement} */ (document.getElementById('dlg')).showModal());
  const [t] = await toast(p, 'inner', 'Copied', 1000);
  assert.ok(t.top, 'shown in the top layer, after the dialog: over it');
  // (the top layer draws in the order things went into it: the toast after the dialog)
  assert.equal(await p.evaluate(() => [...document.querySelectorAll(':modal, :popover-open')].map(e => e.id || e.className).join(' ')), 'dlg copy-toast on');
  await p.close();
});

test('copyText: the Clipboard API, else the old way; false when neither copies', async () => {
  const p = await page();
  const r = await p.evaluate(async () => {
    const w = /** @type {any} */ (window), got = [];
    const clip = ok => Object.defineProperty(navigator, 'clipboard', { configurable: true, value: { writeText: async s => { got.push(['api', s]); if (!ok) throw new Error('no'); } } });
    clip(true);
    const a = await w.C.copyText('one');
    clip(false);
    document.execCommand = cmd => { got.push(['old', cmd]); return true; };
    const b = await w.C.copyText('two');
    document.execCommand = () => false;
    const c = await w.C.copyText('three');
    return { a, b, c, got, left: document.querySelectorAll('textarea').length };
  });
  assert.deepEqual(r, { a: true, b: true, c: false, got: [['api', 'one'], ['api', 'two'], ['old', 'copy'], ['api', 'three']], left: 0 });
  await p.close();
});
