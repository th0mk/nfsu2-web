// The browser deletes the site's data during a game (io-worker.ts recover, shell.ts saveGone), end to end on the start
// page: two profiles (AAA, BBB: copies of a real profile) added with Restore, cloud saves on or off, Play with
// tests/scripts/save_wiped.txt; when it says WIPE (at Options), Storage.clearDataForOrigin clears everything of the
// site (OPFS, localStorage ...), as Chrome's own clearing does; leaving Options then saves the profile the title loaded.
// It reports what the log says of the save and the saves coming back, the save folder before and after (each file's
// size and hash), the cloud's copy (loaded into another save folder with the passphrase), and a second game's load
// of the profile (save_wiped_load.txt).
//   node web/tools/save-wipe.mjs --base http://127.0.0.1:<port> --profile <a real profile file> [--only here|cloud]
// (in WSL: NFSU2_BROWSER=chromium and CHROME_ARGS with the Vulkan/SwiftShader flags, as roam-pair)
// (a development server: the page takes save=, fresh, script=, reg= and env= from its address)
import { chromium } from 'playwright';
import { pickBrowser } from './browser.mjs';
import fs from 'node:fs';

const opt = n => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : null; };
const base = opt('--base') ?? 'http://127.0.0.1:8614', only = opt('--only');
const real = fs.readFileSync(opt('--profile') ?? '');
if (real.length !== 54966) throw new Error('--profile: a profile file of 54,966 bytes');

// a copy of the real profile under another name inside, sealed as the game seals it (profile.ts checksum)
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
const SHOW = /\[save\]|\[app\] the (browser deleted|saves are back|saves could not|game saved profile)|\[write\].*NFS Underground 2|CANNOT OPEN.*\/save|\[expect\]|keyphrase:|\[autotest\].*(WIPE|peek)|TRAP|guest exited|\[app\] the game (exited|could not)|GPU-|renderer/;

/** the files of a save folder: path -> size and SHA-256 (first 12 hex) */
const files = (page, save) => page.evaluate(async save => {
  const out = {};
  async function walk(d, rel) {
    for await (const [name, h] of d.entries()) {
      const r = rel ? `${rel}/${name}` : name;
      if (h.kind === 'directory') await walk(h, r);
      else {
        const b = await (await h.getFile()).arrayBuffer();
        out[r] = `${b.byteLength} ${[...new Uint8Array(await crypto.subtle.digest('SHA-256', b))].slice(0, 6).map(x => x.toString(16).padStart(2, '0')).join('')}`;
      }
    }
  }
  try { await walk(await (await navigator.storage.getDirectory()).getDirectoryHandle(save), ''); } catch (e) { out['(none)'] = String(e); }
  return out;
}, save);
const ready = page => page.waitForFunction(() => window.__nfsu2 && !document.querySelector('#play').disabled, null, { timeout: 180000 });
const played = page => page.waitForFunction(() => window.__nfsu2?.phase === 'exited', null, { timeout: 400000, polling: 500 });
const url = (save, script, fresh) => `${base}/?${new URLSearchParams([['save', save], ...fresh ? [['fresh', '']] : [],
  ['script', `/tests/scripts/${script}`], ['reg', '/tests/web/seed/registry_perf4_native.txt'],
  ['env', 'NFSU2_AUTOSAVE=1'], ['env', 'NFSU2_ROAM=0'], ['env', 'NOMOVIES=1'], ['env', 'NFSU2_FNG_LOG=1'], ['env', 'NOFOCUS=2']])}`;
const show = (what, o) => console.log(`${what}:\n${Object.entries(o).sort().map(([k, v]) => `    ${k}  ${v}`).join('\n')}`);

for (const linked of [false, true]) {
  if (only && only !== (linked ? 'cloud' : 'here')) continue;
  const save = `e2e-wipe-${linked ? 'cloud' : 'here'}`;
  console.log(`\n===== ${linked ? 'cloud saves on' : 'no cloud saves'} (save folder ${save})`);
  const page = await ctx.newPage(), cdp = await ctx.newCDPSession(page);
  let wiped = false, phrase = '';
  const lines = [];
  page.on('console', async m => {
    const t = m.text(); lines.push(t);
    if (opt('--log')) fs.appendFileSync(opt('--log'), t + '\n');
    if (SHOW.test(t)) console.log(`  | ${t}`);
    if (!wiped && /WIPE now/.test(t)) {
      wiped = true;
      await cdp.send('Storage.clearDataForOrigin', { origin: base, storageTypes: 'all' });
      console.log('  >>> the browser cleared the site\'s data (Storage.clearDataForOrigin, all)');
    }
  });
  page.on('pageerror', e => console.log(`  PAGEERROR ${e}`));
  await page.goto(url(save, 'save_wiped.txt', true));
  await ready(page);
  for (const n of ['AAA', 'BBB']) {
    await page.setInputFiles('#import', { name: n, mimeType: 'application/octet-stream', buffer: named(n) });
    await page.waitForSelector('#confirm[open]'); await page.click('#confirm-ok');
    await page.waitForFunction(n => [...document.querySelectorAll('#profiles .name')].some(x => x.textContent === n), n);
  }
  if (linked) {
    await page.waitForSelector('#vault:not([hidden]) #vault-make:not([hidden])', { timeout: 30000 });
    await page.click('#vault-make');
    await page.waitForFunction(() => /^nfsu2-xyz/.test(document.querySelector('#vault-input')?.value ?? ''), null, { timeout: 30000 });
    phrase = await page.inputValue('#vault-input');
    console.log(`  passphrase made (${phrase.slice(0, 12)}...)`);
  }
  show('  the save folder before Play', await files(page, save));
  await page.click('#play');
  await played(page);
  await page.waitForTimeout(10000);                        // the sync at the game's end
  show('  the save folder after the game', await files(page, save));
  if (linked) console.log(`  link kept in localStorage: ${await page.evaluate(s => !!localStorage.getItem(`nfsu2-keyphrase:${s}`), save)}`);
  await page.close();                                      // one tab owns the saves (instance.ts)

  // the cloud's copy, in another save folder of this browser
  if (linked) {
    const p2 = await ctx.newPage();
    p2.on('console', m => { const t = m.text(); if (/keyphrase:/.test(t)) console.log(`  | (check) ${t}`); });
    await p2.goto(`${base}/?save=e2e-wipe-check&fresh`);
    await ready(p2);
    await p2.waitForSelector('#vault:not([hidden])');
    await p2.fill('#vault-input', phrase);
    await p2.click('#vault-load');
    await p2.waitForSelector('#confirm[open]', { timeout: 30000 }); await p2.click('#confirm-ok');
    await p2.waitForFunction(() => document.querySelectorAll('#profiles .name').length >= 2, null, { timeout: 30000 }).catch(() => {});
    show('  the cloud\'s copy, loaded into e2e-wipe-check', await files(p2, 'e2e-wipe-check'));
    await p2.close();
  }

  // the next game loads what was saved
  console.log('  -- the next game');
  const next = await ctx.newPage();
  next.on('console', m => { const t = m.text(); if (SHOW.test(t)) console.log(`  | ${t}`); });
  await next.goto(url(save, 'save_wiped_load.txt', false));
  await ready(next);
  await next.click('#play');
  await played(next);
  show('  the save folder after the next game', await files(next, save));
  await next.close();
}
await browser.close();
