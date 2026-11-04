// The page's menu (F9) in a short window (#44): its buttons take as many columns as the dialog's width gives them, and
// every label is whole, where the two fixed columns cut "Contador de FPS: desactivad…" and left the radio's "R…"; the
// menu of the report's picture (a game, no free roam) is in sight without scrolling on a phone on its side, and the
// free roam menu's own rows (Ghost mode, Crews, Friends, a friend's room) are whole too, scrolling if they must.
//   node --test web/test/menu-layout.test.mjs
// Chrome (or Playwright's Chromium: web/tools/browser.mjs) on this checkout's page (web/dist: node web/build.mjs) with
// its scripts out, so no server and no game: the menu is the page's own.  The labels are the Spanish ones of the
// report's picture, the menu's longest there (Contador de FPS: desactivado, Salir de pantalla completa).
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
const SIZES = [[820, 344, 2, true], [844, 390, 2, true], [1000, 450, 2, true], [450, 1000, 1, true], [320, 568, 1, false]];
/** the menu of the report's picture, and the rows a game in free roam adds (hidden there) */
const REPORT = ['resume', 'fullscreen', 'fps', 'radio', 'help', 'log', 'quit'];
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

/** the menu as the report has it: its rows, their Spanish labels, open over the page */
async function openMenu(page, roam) {
  await page.evaluate(({ roam, report, roamRows }) => {
    const menu = document.querySelector('#menu');
    const row = act => menu.querySelector(`[data-act="${act}"]`);
    const set = (act, text) => { const b = row(act); if (b) b.textContent = text; };
    menu.querySelector('#menu-title').textContent = 'Menú';
    menu.querySelector('.hint').textContent = 'El juego sigue en marcha.';
    menu.querySelector('#menu-save').textContent = 'Todavía no hay nada guardado en esta partida. Último guardado: 19:49 (waza).';
    set('resume', 'Volver al juego');
    set('fullscreen', 'Salir de pantalla completa');
    set('fps', 'Contador de FPS: desactivado');
    set('solid', 'Modo fantasma: desactivado');
    set('crews', 'Equipos: ninguno');
    set('friends', 'Amigos');
    set('friend', 'Unirse a la sala de Ana');
    set('help', 'Controles');
    set('log', 'Descargar registro');
    set('quit', 'Salir del juego');
    menu.querySelector('#radio-label').textContent = 'Radio: desactivado';
    for (const act of report) row(act).hidden = false;
    for (const act of roamRows) row(act).hidden = !roam;
    if (!menu.open) menu.showModal();
  }, { roam, report: REPORT, roamRows: ROAM });
}

/** what the page says of the open menu: the labels it cuts, its columns, and whether it needs scrolling */
async function measure(page) {
  return page.evaluate(() => {
    const menu = document.querySelector('#menu');
    const clipped = [];
    for (const b of menu.querySelectorAll('.menu-actions > .btn'))
      if (b.getClientRects().length && b.scrollWidth > b.clientWidth + 1) clipped.push(b.textContent.trim());
    const radio = menu.querySelector('.menu-radio .btn span');
    if (radio.scrollWidth > radio.clientWidth + 1) clipped.push('Radio: ' + radio.textContent.trim());
    const cols = getComputedStyle(menu.querySelector('.menu-actions')).gridTemplateColumns.split(' ').length;
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
  } finally {
    await browser.close();
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
