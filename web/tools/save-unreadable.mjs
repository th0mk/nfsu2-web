// Play with a save folder that does not answer, on the real page (a player's Firefox 159, Discord 2026-11-04: every call
// there answered "UnknownError: Unknown failure"; the page took settings.ini for missing, tried to write the template
// over it, and Play stopped with that bare error).  The page's own thread gets Firefox's answer for the save folder's
// files while window.__fail is on (the game's workers are left as they are: the check runs before them).  Checks: the
// start page says so in Saves and writes nothing; Play asks (what the browser answered, what to do, Play without saving,
// SAVES-UNREADABLE), and Cancel brings the start page back saying it; settings.ini, the registry and the profile stay
// byte for byte.  Play without saving: the game starts, says so, and runs with /save in memory (its registry written
// there, F12 keeping no screenshot); the save folder after it is as before, byte for byte, while a healthy game writes
// its registry.  A folder that answers again within the check's retries starts the game; a healthy one at once.
//   node web/tools/save-unreadable.mjs --base http://127.0.0.1:<port> [--firefox]      (a --dev server)
// --firefox: Playwright's Firefox, headed (headless Firefox has no WebGPU adapter, and Play needs one).  In WSL:
// NFSU2_BROWSER=chromium CHROME_ARGS="--ignore-gpu-blocklist --enable-features=Vulkan,VulkanFromANGLE,DefaultANGLEVulkan
// --use-angle=vulkan --enable-unsafe-swiftshader" (without them SwiftShader's WebGPU stops the game within a second).
import { chromium, firefox } from 'playwright';
import { pickBrowser } from './browser.mjs';

const opt = n => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : null; };
const base = opt('--base') ?? 'http://127.0.0.1:8614', SAVE = 'e2e-unreadable';
const browser = process.argv.includes('--firefox')
  ? await firefox.launch({ headless: false, firefoxUserPrefs: { 'dom.webgpu.enabled': true, 'gfx.webgpu.ignore-blocklist': true, 'media.autoplay.default': 0 } })
  : await chromium.launch({ ...pickBrowser(), headless: true,
    args: ['--enable-unsafe-webgpu', '--autoplay-policy=no-user-gesture-required', ...(process.env.CHROME_ARGS ? process.env.CHROME_ARGS.split(' ') : [])] });
console.log(`${browser.browserType().name()} ${browser.version()}`);
const ctx = await browser.newContext({ viewport: { width: 1280, height: 800 } });
await ctx.addInitScript(SAVE => {
  const names = new Set(['settings.ini', 'registry.txt', 'AAA', `${SAVE}~check`]);
  for (const m of ['getFile', 'createWritable']) {
    const orig = FileSystemFileHandle.prototype[m];
    FileSystemFileHandle.prototype[m] = function (...a) {
      return window.__fail && names.has(this.name) ? Promise.reject(new DOMException('Unknown failure', 'UnknownError')) : orig.apply(this, a);
    };
  }
  window.__fail = sessionStorage.getItem('fail') === '1';
}, SAVE);
const results = [];
const check = (what, ok, detail = '') => { results.push(ok); console.log(`${ok ? 'PASS' : 'FAIL'}: ${what}${detail ? ` (${detail})` : ''}`); };
const page = await ctx.newPage();
const errors = [];
page.on('pageerror', e => errors.push(String(e)));
// (Playwright's Firefox: no full screen, whose request holds its click on Play for 30 s)
const url = `${base}/?save=${SAVE}&env=NOMOVIES%3D1&env=NFSU2_ROAM%3D0${process.argv.includes('--firefox') ? '&env=NFSFULL%3D0' : ''}`;
const ready = () => page.waitForFunction(() => window.__nfsu2 && !document.querySelector('#play').disabled, null, { timeout: 180000 });
const logText = () => page.evaluate(() => window.__nfsu2.log.text());
/** the save folder's files: path -> bytes as text (the profile: its length and first byte), read with __fail off */
const files = () => page.evaluate(async SAVE => {
  const was = window.__fail; window.__fail = false;
  const out = {};
  async function walk(d, rel) {
    for await (const [n, h] of d.entries()) {
      const r = rel ? `${rel}/${n}` : n;
      if (h.kind === 'directory') await walk(h, r);
      else { const b = new Uint8Array(await (await h.getFile()).arrayBuffer()); out[r] = n === 'AAA' ? `${b.length} bytes of ${b[0]}` : new TextDecoder().decode(b); }
    }
  }
  try { await walk(await (await navigator.storage.getDirectory()).getDirectoryHandle(SAVE), ''); } finally { window.__fail = was; }
  return out;
}, SAVE);

// the saves: a settings.ini of the player's own, a registry, a profile's file
await page.goto(url);
await ready();
// (the page's own first start ends with its template written: after that, or it lands over the seed)
await page.waitForFunction(() => window.__nfsu2.settingsText() !== null, null, { timeout: 30000 });
await page.evaluate(async SAVE => {
  const root = await navigator.storage.getDirectory();
  await root.removeEntry(SAVE, { recursive: true }).catch(() => {});
  const d = await root.getDirectoryHandle(SAVE, { create: true });
  const put = async (dir, name, data) => { const w = await (await dir.getFileHandle(name, { create: true })).createWritable(); await w.write(data); await w.close(); };
  await put(d, 'settings.ini', '# my own\nintro = off\nhorn = train\nshow_hidden_races = on\n');
  await put(d, 'registry.txt', 'd PowerUnits=1\nd g_RacingResolution=4\n');
  const p = await (await d.getDirectoryHandle('NFS Underground 2', { create: true })).getDirectoryHandle('AAA', { create: true });
  await put(p, 'AAA', new Uint8Array(54966).fill(7));
}, SAVE);
const before = await files();
if (!before['settings.ini']?.startsWith('# my own')) throw new Error(`the seed did not hold: ${JSON.stringify(before).slice(0, 200)}`);

// 1. the folder does not answer from the page's start: said, nothing written; Play asks, Cancel brings the page back
await page.evaluate(() => sessionStorage.setItem('fail', '1'));
await page.reload();
await ready();
await page.waitForTimeout(2000);
let log = await logText();
check('the start says settings.ini did not read, and leaves it', /\[app\] settings\.ini could not be read: UnknownError: Unknown failure; left as it is/.test(log));
check('no template written', !/wrote template/.test(log));
const savesMsg = await page.textContent('#saves-msg');
check('Saves says the browser cannot read the saves', /can't read or write your saves right now \(UnknownError: Unknown failure\)/.test(savesMsg ?? ''), savesMsg ?? '');
const ADVICE = /can't read or write your saves right now \(UnknownError: Unknown failure\)\. Restart the browser and check that the disk has free space, then try again\. If it still fails, don't remove this site's data: your saves are only in this browser\./;
const t0 = Date.now();
await page.click('#play');
await page.waitForSelector('#confirm[open]', { timeout: 60000 });
const title = await page.textContent('#confirm-title'), body = await page.textContent('#confirm-body');
console.log(`  asked after ${((Date.now() - t0) / 1000).toFixed(1)} s: ${title} | ${body}`);
check('Play asks: what the browser answered, what to do, Play without saving, the code', title === 'Play without saving?' && ADVICE.test(body ?? '') &&
  /Play without saving starts the game, but nothing you do in it is kept\./.test(body ?? '') && /SAVES-UNREADABLE/.test(body ?? '') &&
  await page.textContent('#confirm-ok') === 'Play without saving' && await page.textContent('#confirm-cancel') === 'Cancel');
check('its two buttons plain and equal, Cancel focused (Enter keeps the page)', await page.evaluate(() =>
  document.querySelector('#confirm-ok').className === 'btn' && document.querySelector('#confirm-cancel').className === 'btn' && document.activeElement?.id === 'confirm-cancel'));
await page.click('#confirm-cancel');
await page.waitForFunction(() => window.__nfsu2.phase === 'exited', null, { timeout: 60000 });
const status = await page.textContent('#status'), sub = await page.textContent('#status-sub');
check('Cancel: did not start, with what to do and the code', status === 'Did not start' && ADVICE.test(sub ?? '') && /SAVES-UNREADABLE/.test(sub ?? ''), sub ?? '');
check('no game, the stage down, the page usable', await page.evaluate(() => !window.__nfsu2.session && document.querySelector('#stage').hidden && !document.querySelector('#app').inert));
log = await logText();
check('the log says the problem in English with its code', /\[app\] problem SAVES-UNREADABLE: the save folder does not answer \(read: UnknownError: Unknown failure; 4 tries\)/.test(log) &&
  /\[app\] not played without saving/.test(log) && /\[app\] the game did not start \(SAVES-UNREADABLE\), nothing written/.test(log));
check('the saves are as they were, byte for byte', JSON.stringify(await files()) === JSON.stringify(before));
check('nothing left beside the save folder', !(await page.evaluate(async S => { for await (const n of (await navigator.storage.getDirectory()).keys()) if (n.startsWith(`${S}~check`)) return true; return false; }, SAVE)));

// 2. Play without saving: the game starts and says so, runs (its registry written at its start: into memory), F12
// keeps no screenshot; the save folder after it as before, byte for byte
// (the game's start: its registry read, then written a moment later, at its first RegSetValue: runtime/win32_ext.c)
const booted = async () => page.waitForFunction(n => window.__nfsu2.log.text().split('\n').slice(n).some(l => /\[registry\] registry file/.test(l)),
  (await logText()).split('\n').length, { timeout: 120000, polling: 500 }).then(() => page.waitForTimeout(8000));
await page.click('#play');
await page.waitForSelector('#confirm[open]', { timeout: 60000 });
const bootWait = booted();
await page.click('#confirm-ok');
await page.waitForFunction(() => window.__nfsu2.phase === 'playing', null, { timeout: 120000 });
check('the game says, as it starts, that nothing is kept', await page.waitForFunction(() => document.querySelector('#stage').innerText.includes('Playing without saving: nothing from this game is kept.'), null, { timeout: 10000 }).then(() => true, () => false));
await bootWait;
await page.waitForTimeout(3000);
await page.keyboard.press('F12');
await page.waitForTimeout(2000);
log = await logText();
check('its /save is the save folder\'s files in memory', /\[save\] playing without saving: \/save is in this game's memory, 3 files of the save folder copied in/.test(log) &&
  /\[app\] autosave: playing without saving/.test(log));
if (process.env.DBG) console.log(log.split('\n').filter(l => /\[save\]|\[app\]|exit|trap|TRAP|registry|CANNOT|error/i.test(l)).slice(-60).join('\n'));
await page.evaluate(() => window.__nfsu2.session?.terminate());
await page.waitForFunction(() => window.__nfsu2.phase === 'exited', null, { timeout: 60000 });
check('after it, the saves are as they were, byte for byte (no registry, settings, profile or screenshot written)', JSON.stringify(await files()) === JSON.stringify(before),
  JSON.stringify(Object.fromEntries(Object.entries(await files()).map(([k, v]) => [k, v.slice(0, 40)]))));

// 3. it answers again during the check's retries: the game starts, nothing asked
await page.evaluate(() => { window.__fail = true; setTimeout(() => { window.__fail = false; sessionStorage.removeItem('fail'); }, 700); });
await page.click('#play');
await page.waitForFunction(() => window.__nfsu2.phase === 'playing', null, { timeout: 120000 });
log = await logText();
check('a folder that answers again within the retries starts the game', /\[app\] the save folder answered after [234] tries/.test(log));
await page.evaluate(() => window.__nfsu2.session.terminate());
await page.waitForFunction(() => window.__nfsu2.phase === 'exited', null, { timeout: 60000 });

// 4. healthy: the game starts at once, the check says nothing, and the game writes its registry (what 2 kept out)
const n0 = (await logText()).split('\n').length;
const bootWait2 = booted();
await page.click('#play');
await page.waitForFunction(() => window.__nfsu2.phase === 'playing', null, { timeout: 120000 });
const fresh = (await logText()).split('\n').slice(n0 - 1).join('\n');
check('a healthy folder starts the game, the check silent', !/save folder (answered|does not answer)|SAVES-UNREADABLE|playing without saving/.test(fresh));
await bootWait2;
await page.waitForTimeout(3000);
await page.evaluate(() => window.__nfsu2.session.terminate());
await page.waitForFunction(() => window.__nfsu2.phase === 'exited', null, { timeout: 60000 });
const after = await files();
check('the saves the games found are there after them, the registry written by this one', after['settings.ini']?.startsWith('# my own') &&
  after['NFS Underground 2/AAA/AAA'] === before['NFS Underground 2/AAA/AAA'] && after['registry.txt'] !== before['registry.txt'],
  JSON.stringify(Object.fromEntries(Object.entries(after).map(([k, v]) => [k, v.slice(0, 40)]))));
check('no page errors', !errors.length, errors.join(' | '));
if (process.env.DBG) console.log((await logText()).split('\n').filter(l => /settings|template|=== NFSU2|save folder|autosave|could not/.test(l)).join('\n'));

await page.evaluate(async S => (await navigator.storage.getDirectory()).removeEntry(S, { recursive: true }).catch(() => {}), SAVE);
await browser.close();
process.exitCode = results.every(Boolean) ? 0 : 1;
console.log(results.every(Boolean) ? 'ALL PASS' : 'SOME FAILED');
