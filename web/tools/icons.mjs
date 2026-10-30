// A game's app icons and favicon from its one mark: a dark rounded square, and in it a smaller rounded square in the
// game's colour (Underground 2's: web/src/static and index.html's favicon).
//   node web/tools/icons.mjs --accent '#c8f088' [--fill '#404038'] --out <folder>
// writes into <folder>: icon-192.png and icon-512.png (the mark, its corners clear), icon-180.png (the home-screen
// icon of an iPhone: full bleed, the system rounds it), icon-maskable-512.png (full bleed, the mark in the middle 72 %,
// inside the safe zone), and favicon.txt: the favicon's data: URL for the game's head.  Drawn by Chrome or Playwright's
// Chromium (web/tools/browser.mjs).
import fs from 'node:fs';
import path from 'node:path';
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';

const arg = (k, d) => { const i = process.argv.indexOf(k); return i < 0 ? d : process.argv[i + 1]; };
const ACCENT = arg('--accent'), FILL = arg('--fill', '#404038'), BG = arg('--bg', '#171717'), OUT = arg('--out');
if (!/^#[0-9a-f]{6}$/i.test(ACCENT ?? '') || !/^#[0-9a-f]{6}$/i.test(FILL) || !/^#[0-9a-f]{6}$/i.test(BG) || !OUT) {
  console.error('usage: node web/tools/icons.mjs --accent #rrggbb [--fill #rrggbb] [--bg #rrggbb] --out <folder>');
  process.exit(2);
}

/** the mark in a 32-unit square: its rounded background (rx 8; none: full bleed) and the square in it */
const mark = (round = true) => `<rect width="32" height="32"${round ? ' rx="8"' : ''} fill="${BG}"/>` +
  `<rect x="5" y="5" width="22" height="22" rx="6" fill="${FILL}" stroke="${ACCENT}" stroke-width="2"/>`;
const svg = (body, view = '0 0 32 32') => `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${view}">${body}</svg>`;
// the maskable one: full bleed, the mark (without its own background's corners) at 72 % in the middle
const S = 32 / 0.72, O = (S - 32) / 2;
const ICONS = {
  'icon-192.png': [192, svg(mark())],
  'icon-512.png': [512, svg(mark())],
  'icon-180.png': [180, svg(mark(false))],
  'icon-maskable-512.png': [512, svg(`<rect x="${-O}" y="${-O}" width="${S}" height="${S}" fill="${BG}"/>${mark(false)}`, `${-O} ${-O} ${S} ${S}`)],
};

fs.mkdirSync(OUT, { recursive: true });
const browser = await chromium.launch({ ...pickBrowser(), headless: true });
try {
  for (const [name, [px, s]] of Object.entries(ICONS)) {
    const page = await browser.newPage({ viewport: { width: px, height: px }, deviceScaleFactor: 1 });
    await page.setContent(`<!doctype html><style>html,body{margin:0;background:transparent}svg{display:block;width:${px}px;height:${px}px}</style>${s}`);
    fs.writeFileSync(path.join(OUT, name), await page.screenshot({ omitBackground: true, clip: { x: 0, y: 0, width: px, height: px } }));
    await page.close();
    console.log(`${path.join(OUT, name)}: ${px} x ${px}`);
  }
} finally {
  await browser.close();
}
// the favicon as index.html writes it: the same mark, its characters escaped for an attribute's data: URL
const fav = 'data:image/svg+xml,' + svg(mark()).replace(/"/g, '\'').replace(/[<>#]/g, c => encodeURIComponent(c));
fs.writeFileSync(path.join(OUT, 'favicon.txt'), fav + '\n');
console.log(`${path.join(OUT, 'favicon.txt')}: the favicon's data: URL`);
