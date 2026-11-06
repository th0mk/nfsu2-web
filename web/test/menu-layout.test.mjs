// The page's menu (F9) in a short window (#44): a list, what each item is on the left and its state on the right, in
// two blocks that stand side by side where the window is short; every label is whole (the old buttons cut "Contador de
// FPS: desactivad…" and left the radio's "R…"), and on a phone on its side the whole menu is in sight without
// scrolling, free roam's rows (Ghost mode, Crews, Friends, a friend's request, the line's way out) too.
//   node --test web/test/menu-layout.test.mjs
// Chrome (or Playwright's Chromium: web/tools/browser.mjs) on this checkout's page (web/dist: node web/build.mjs) with
// its scripts out, so no server and no game: the menu is the page's own.  The words are the Spanish ones of the
// report's picture, the menu's longest there.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { chromium } from 'playwright';
import { pickBrowser } from '../tools/browser.mjs';

const WEB = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const DIST = path.join(WEB, 'dist');
const SKIP = !fs.existsSync(path.join(DIST, 'main.js')) ? 'no page built (node web/build.mjs)' : false;
/** the sizes measured: [width, height, the least columns, the whole menu in sight] */
const SIZES = [[820, 344, 2, true], [844, 390, 2, true], [1000, 450, 2, true], [450, 1000, 1, true], [320, 568, 1, true]];
/** the menu of the report's picture, and the rows a game in free roam adds (hidden there) */
const REPORT = ['resume', 'fullscreen', 'radio', 'help', 'log', 'quit'];
const ROAM = ['solid', 'crews', 'friends', 'friend', 'offline'];

/** the page, its scripts out, beside the built stylesheet */
function pageDir() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'nfsu2-menu-test-'));
  const html = fs.readFileSync(path.join(WEB, 'index.html'), 'utf8')
    .replace('href="/main.css"', 'href="main.css"')
    .replace(/<script[^>]*><\/script>/g, '');
  fs.writeFileSync(path.join(dir, 'index.html'), html);
  fs.copyFileSync(path.join(DIST, 'main.css'), path.join(dir, 'main.css'));
  return dir;
}

/** the menu as the report has it: its rows, their Spanish words, open over the page */
async function openMenu(page, roam) {
  await page.evaluate(({ roam, report, roamRows }) => {
    const menu = document.querySelector('#menu');
    const row = act => menu.querySelector(`[data-act="${act}"]`);
    /** a row's words: what it is, and its state on the right (a row of one text: the text) */
    const set = (act, text, value) => {
      const b = row(act);
      if (value === undefined) { b.textContent = text; return; }
      b.firstElementChild.textContent = text; b.querySelector('.menu-val').textContent = value;
    };
    menu.querySelector('#menu-title').textContent = 'Menú';
    menu.querySelector('#menu-save').textContent = 'Todavía no hay nada guardado en esta partida. Último guardado: 19:49 (waza).';
    set('resume', 'Cerrar');
    set('fullscreen', 'Pantalla completa', 'Activado');
    set('solid', 'Modo fantasma', 'Desactivado');
    set('crews', 'Equipos', '');
    set('friends', 'Amigos', '1');
    set('friend', 'Aceptar a Anastasia como amiga');
    set('offline', 'Dejar de esperar: jugar sin conexión');
    set('radio', 'Radio', 'Desactivado');
    set('help', 'Controles');
    set('log', 'Descargar registro');
    set('quit', 'Salir del juego');
    for (const act of report) row(act).hidden = false;
    for (const act of roamRows) row(act).hidden = !roam;
    if (!menu.open) menu.showModal();
  }, { roam, report: REPORT, roamRows: ROAM });
}

/** what the page says of the open menu: the words it cuts, its columns, and whether it needs scrolling */
async function measure(page) {
  return page.evaluate(() => {
    const menu = document.querySelector('#menu');
    const clipped = [];
    for (const e of menu.querySelectorAll('.menu-row, .menu-row > span'))
      if (e.getClientRects().length && e.scrollWidth > e.clientWidth + 1) clipped.push(e.textContent.trim());
    const cols = getComputedStyle(menu.querySelector('.menu-body')).gridTemplateColumns.split(' ').filter(c => c !== 'none').length || 1;
    return { clipped, cols, fits: menu.scrollHeight <= menu.clientHeight + 1 };
  });
}

test('the page menu fits a short window and cuts no label', { skip: SKIP }, async () => {
  const dir = pageDir();
  const browser = await chromium.launch({ ...pickBrowser(), headless: true });
  try {
    const page = await browser.newPage({ viewport: { width: SIZES[0][0], height: SIZES[0][1] } });
    await page.goto(pathToFileURL(path.join(dir, 'index.html')).href);
    await openMenu(page, false);
    for (const [w, h, columns, fits] of SIZES) {
      await page.setViewportSize({ width: w, height: h });
      const r = await measure(page);
      assert.deepEqual(r.clipped, [], `${w}x${h}: every label whole`);
      assert.ok(r.cols >= columns, `${w}x${h}: ${r.cols} column(s), at least ${columns}`);
      if (fits) assert.ok(r.fits, `${w}x${h}: the whole menu in sight (no scrolling)`);
    }
    // the free roam rows too: whole labels, in as many columns
    await page.setViewportSize({ width: 820, height: 344 });
    await openMenu(page, true);
    const roam = await measure(page);
    assert.deepEqual(roam.clipped, [], '820x344 free roam: every label whole');
    assert.ok(roam.cols >= 2, `820x344 free roam: ${roam.cols} column(s)`);
    await page.setViewportSize({ width: 844, height: 390 });
    assert.ok((await measure(page)).fits, '844x390 free roam: the whole menu in sight');
  } finally {
    await browser.close();
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
