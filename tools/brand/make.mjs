// The logo files and the site's icons (tools/brand/draw.ts), drawn by Chrome in the lettering of the mark on F12
// pictures (web/src/gpu/mark.ts).  Run it on Windows, where that lettering is Arial Bold Italic, from a checkout's
// root (its web/node_modules: esbuild, playwright with the installed Chrome):
//   node tools/brand/make.mjs
// It writes, in the repo it is in: tools/brand/logo.png (the logo, see-through, with the mark's shadow),
// logo-dark.png (for light backgrounds), discord-icon.png (1024 x 1024), discord-banner.png (1920 x 1080), and the
// start page's web/src/static/favicon-16.png, favicon-32.png, icon-180.png, icon-192.png, icon-512.png and
// icon-maskable-512.png.
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url)), root = path.resolve(here, '../..');
const req = createRequire(path.join(process.cwd(), 'web', 'package.json'));
const esbuild = req('esbuild'), { chromium } = req('playwright');

const js = (await esbuild.build({ entryPoints: [path.join(here, 'draw.ts')], bundle: true, format: 'iife', write: false, logLevel: 'error' })).outputFiles[0].text;
const browser = await chromium.launch({ channel: 'chrome' });
try {
  const page = await browser.newPage();
  await page.setContent('<!doctype html><meta charset="utf-8"><body></body>');
  await page.addScriptTag({ content: js });
  const files = await page.evaluate(() => window.brand());
  for (const [name, url] of files) {
    const f = path.join(root, name), png = Buffer.from(url.slice(url.indexOf(',') + 1), 'base64');
    fs.writeFileSync(f, png);
    console.log(`${name}  ${png.length} bytes`);
  }
} finally { await browser.close(); }
