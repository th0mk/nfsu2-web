// save-wipe.mjs's wipe with the cloud behind this browser (review of 502d7e8f): AAA and BBB sent with a new passphrase,
// then AAC made here and BBB changed while the server refuses sends (507: as a player's VAULT-TOO-LARGE, or no
// connection), the server answering again at Play.  The site's data cleared at Options (save_wiped.txt), the game saves
// BBB.  Safe: AAC is still here after the game, and in the cloud.  On 502d7e8f the refill takes the cloud's older copy:
// AAC is gone from the save folder and from the cloud (and autosave-profiles.txt, which the cloud never had).
//   node web/tools/save-wipe-stale.mjs --base http://127.0.0.1:<port> --profile <a real profile file> [--log <file>]
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
function named(name, money) {
  const f = Buffer.from(real);
  if (money !== undefined) f.writeUInt32LE(money, 0xA16A);
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


{
  const save = 'e2e-wipe-stale';
  console.log(`\n===== cloud saves on, the cloud behind this browser (save folder ${save})`);
  const page = await ctx.newPage(), cdp = await ctx.newCDPSession(page);
  let wiped = false;
  page.on('console', async m => {
    const t = m.text();
    if (opt('--log')) fs.appendFileSync(opt('--log'), t + '\n');
    if (SHOW.test(t) || /Sync|vault|toast/i.test(t)) console.log(`  | ${t}`);
    if (!wiped && /WIPE now/.test(t)) {
      wiped = true;
      await cdp.send('Storage.clearDataForOrigin', { origin: base, storageTypes: 'all' });
      console.log('  >>> the browser cleared the site\'s data (Storage.clearDataForOrigin, all)');
    }
  });
  page.on('pageerror', e => console.log(`  PAGEERROR ${e}`));
  await page.goto(url(save, 'save_wiped.txt', true));
  await ready(page);
  const add = async (n, money) => {
    await page.setInputFiles('#import', { name: n, mimeType: 'application/octet-stream', buffer: named(n, money) });
    await page.waitForSelector('#confirm[open]'); await page.click('#confirm-ok');
    await page.waitForFunction(n => [...document.querySelectorAll('#profiles .name')].some(x => x.textContent === n), n);
  };
  for (const n of ['AAA', 'BBB']) await add(n);
  await page.waitForSelector('#vault:not([hidden]) #vault-make:not([hidden])', { timeout: 30000 });
  await page.click('#vault-make');
  await page.waitForFunction(() => /^nfsu2-xyz/.test(document.querySelector('#vault-input')?.value ?? ''), null, { timeout: 30000 });
  const phrase = await page.inputValue('#vault-input');
  console.log(`  passphrase made (${phrase.slice(0, 12)}...): the cloud has AAA and BBB`);
  // the server refuses sends for a while (507: no room; the same as a player's VAULT-TOO-LARGE or a lost connection)
  let refused = 0;
  await page.route('**/vault', r => { if (r.request().method() === 'PUT') { refused++; return r.fulfill({ status: 507, body: 'full' }); } return r.continue(); });
  await add('AAC');
  await page.waitForTimeout(1500);
  await add('BBB', 123456);                                // BBB changed (newest, last by name): the title loads BBB
  await page.waitForTimeout(8000);
  console.log(`  AAC made here; sends refused by the server: ${refused}; the section says: ${JSON.stringify(await page.textContent('#vault-status'))}`);
  await page.unroute('**/vault');
  show('  the save folder before Play', await files(page, save));
  await page.click('#play');
  await played(page);
  await page.waitForTimeout(12000);                        // the sync at the game's end
  const after = await files(page, save);
  show('  the save folder after the game', after);
  console.log(`  section: ${JSON.stringify(await page.textContent('#vault-status'))}`);
  await page.close();
  const p2 = await ctx.newPage();
  await p2.goto(`${base}/?save=e2e-wipe-stale-check&fresh`);
  await ready(p2);
  await p2.waitForSelector('#vault:not([hidden])');
  await p2.fill('#vault-input', phrase);
  await p2.click('#vault-load');
  await p2.waitForSelector('#confirm[open]', { timeout: 30000 }); await p2.click('#confirm-ok');
  await p2.waitForTimeout(3000);
  const cloud = await files(p2, 'e2e-wipe-stale-check');
  show('  the cloud\'s copy now, loaded into e2e-wipe-stale-check', cloud);
  await p2.close();
  const ok = !!after['NFS Underground 2/AAC/AAC'] && !!cloud['NFS Underground 2/AAC/AAC'];
  console.log(ok ? 'PASS: AAC, made here while the cloud refused sends, is still here and in the cloud'
    : 'FAIL: AAC, made here while the cloud refused sends, is lost (the refill took the cloud\'s older copy)');
  process.exitCode = ok ? 0 : 1;
}
await browser.close();
