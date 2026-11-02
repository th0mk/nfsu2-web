// The player is told what came back after the browser deleted the site's data during a game (shell.ts saveGone's
// toast, 15 s), end to end: save-wipe.mjs's run (two profiles, no cloud saves, the data cleared at Options with
// tests/scripts/save_wiped.txt), with every text the in-game toast shows recorded, and when.  Safe: the toast saying
// the saves are back stays at least 3 s.  On f4e3f301 the game's own save, which the recovery held and lets go right
// after, replaces it with "Progress saved" within milliseconds: the player never reads it (nor the one saying the saves
// could not all be put back).
//   node web/tools/save-wipe-toast.mjs --base http://127.0.0.1:<port> --profile <a real profile file>
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import fs from 'node:fs';

const opt = n => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : null; };
const base = opt('--base') ?? 'http://127.0.0.1:8614';
const real = fs.readFileSync(opt('--profile') ?? '');
if (real.length !== 54966) throw new Error('--profile: a profile file of 54,966 bytes');
const T = new Uint32Array(256);
for (let i = 0; i < 256; i++) { let r = i << 24; for (let k = 0; k < 8; k++) r = r & 0x80000000 ? (r << 1) ^ 0x04C11DB7 : r << 1; T[i] = r >>> 0; }
const checksum = d => { let r = ~((d[0] << 24) | (d[1] << 16) | (d[2] << 8) | d[3]) >>> 0; for (let i = 4; i < d.length; i++) r = (((r << 8) | d[i]) ^ T[r >>> 24]) >>> 0; return ~r >>> 0; };
function named(name) {
  const f = Buffer.from(real);
  f.fill(0, 36 + 53761, 36 + 53761 + 32); f.write(name, 36 + 53761, 'latin1');
  f.writeUInt32LE(checksum(f.subarray(28, 36)), 16); f.writeUInt32LE(checksum(f.subarray(36)), 20); f.writeUInt32LE(checksum(f.subarray(0, 24)), 24);
  return f;
}

const browser = await chromium.launch({ ...pickBrowser(), headless: true,
  args: ['--enable-unsafe-webgpu', '--autoplay-policy=no-user-gesture-required', ...(process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [])] });
const ctx = await browser.newContext({ viewport: { width: 1280, height: 720 }, bypassCSP: true });
await ctx.addInitScript(() => {
  window.__toasts = [];
  document.addEventListener('DOMContentLoaded', () => {
    const t = document.querySelector('#toast'); if (!t) return;
    new MutationObserver(() => window.__toasts.push({ at: performance.now(), text: t.hidden ? '' : t.textContent }))
      .observe(t, { childList: true, characterData: true, subtree: true, attributes: true });
  });
});
const page = await ctx.newPage(), cdp = await ctx.newCDPSession(page);
let wiped = false;
page.on('console', async m => {
  const t = m.text();
  if (/\[save\]|\[app\] the (saves|browser)/.test(t)) console.log(`  | ${t}`);
  if (!wiped && /WIPE now/.test(t)) { wiped = true; await cdp.send('Storage.clearDataForOrigin', { origin: base, storageTypes: 'all' }); console.log('  >>> the site\'s data cleared'); }
});
const save = 'e2e-wipe-toast';
await page.goto(`${base}/?${new URLSearchParams([['save', save], ['fresh', ''], ['script', '/tests/scripts/save_wiped.txt'],
  ['reg', '/tests/web/seed/registry_perf4_native.txt'], ['env', 'NFSU2_AUTOSAVE=1'], ['env', 'NFSU2_ROAM=0'], ['env', 'NOMOVIES=1'], ['env', 'NOFOCUS=2']])}`);
await page.waitForFunction(() => window.__nfsu2 && !document.querySelector('#play').disabled, null, { timeout: 180000 });
for (const n of ['AAA', 'BBB']) {
  await page.setInputFiles('#import', { name: n, mimeType: 'application/octet-stream', buffer: named(n) });
  await page.waitForSelector('#confirm[open]'); await page.click('#confirm-ok');
  await page.waitForFunction(n => [...document.querySelectorAll('#profiles .name')].some(x => x.textContent === n), n);
}
await page.click('#play');
await page.waitForFunction(() => window.__nfsu2?.phase === 'exited', null, { timeout: 400000, polling: 500 });
const seen = await page.evaluate(() => window.__toasts);
await browser.close();
// each text, for how long it showed
const shown = [];
for (const x of seen) if (!shown.length || shown.at(-1).text !== x.text) shown.push({ ...x });
for (let i = 0; i < shown.length; i++) shown[i].ms = i + 1 < shown.length ? Math.round(shown[i + 1].at - shown[i].at) : null;
console.log('the toast showed:');
for (const s of shown) if (s.text) console.log(`  ${s.ms === null ? '(to the end)' : `${s.ms} ms`}: ${s.text}`);
const back = shown.find(s => /deleted this site's data during the game/.test(s.text));
const ok = !!back && (back.ms === null || back.ms >= 3000);
console.log(ok ? `PASS: the player is told the saves are back (${back.ms ?? 'to the end'} ms)`
  : back ? `FAIL: the toast saying the saves are back showed ${back.ms} ms, then "${shown[shown.indexOf(back) + 1]?.text}" replaced it` : 'FAIL: no toast said the saves are back');
process.exitCode = ok ? 0 : 1;
