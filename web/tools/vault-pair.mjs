// Keyphrase saves between two browsers (web/src/app/vault-ui.ts, the server's /vault): two Chrome contexts (each its
// own OPFS and localStorage) on the start page of a running server.
//
//   node web/tools/vault-pair.mjs --base http://127.0.0.1:8080 --backup <a "Download a backup" zip> [--state <dir>] [--play] [--headed]
//        [--game ug2|mw] (the game's saves; Most Wanted has none yet) [--help] (this text and the game's rounds)
//
// A restores the backup and makes a keyphrase; B (with a screenshot of its own) types it in, in another shape, and
// loads the saves, but A's DVD and magazine covers (a keyphrase keeps none: each 1 MB, the server takes 256 KB) and
// the device's own (B keeps its registry.txt and its resolution scale and frame rate in settings.ini); A's resolution
// scale changed sends nothing; a settings.ini changed in A comes to B, a cover B has of its own and B's scale staying;
// A's LAN server's file (gamefile.bin, 300 KB that do not pack: past the cap on its own) never goes, B keeps its own
// and A's changing sends nothing; a rename in A comes to B at its next page
// load; a send of A's the server kept without A hearing back is no conflict at A's next send (issue 33); both rename
// (a conflict, both sides shown: B keeps its own when asked, A takes it at its next load); A deletes the saves from the server
// and B is told at its next load.  Before that, A as the page before 2026-10-28 linked it (version 3, its hash
// over gamefile.bin too): unchanged, it takes the hash of now and sends nothing; its gamefile.bin changed since, a
// rename goes (the page of then failed past the cap there).  The profile's bytes are
// compared between the two, B's screenshot must stay throughout.  --state: the server's state folder as this machine
// reads it (\\wsl.localhost\... from Windows): exactly one keyphrase file in its vault folder, which holds neither the
// profile's name nor a byte of the profile.  --play (a server with --dev and a module): two more browsers on fresh
// save folders, C with a keyphrase of no profile plays tests/scripts/app_rw_probe.txt (a new profile ROTEST, saved,
// a Quick Race, the game's own close): the save goes to the server during the game, 10 s after it; D loads it,
// the same bytes, and renames it; C's next Play takes the rename before the game starts.  Prints each step, PASS or
// FAIL at the end (exit code 1).
import { chromium } from 'playwright';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { gameOf, usage, nothingFor, requireGame } from './rig-game.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
// each game's rounds: what the steps below do with its saves (its profile folder, its backup, its probe script)
const ROUNDS_OF = {
  ug2: ['A and B: the backup\'s saves (--backup, a "Download a backup" zip of Underground 2 saves) through a keyphrase',
    '--play: C and D on fresh saves, tests/scripts/app_rw_probe.txt (a new profile ROTEST, a Quick Race)'],
};
const NONE = 'none yet: no Most Wanted saves to send through a keyphrase (M6-29)';
const GAME = gameOf(process.argv.slice(2));
if (process.argv.includes('--help')) usage(import.meta.url, GAME, 'Rounds', ROUNDS_OF[GAME] ?? [], NONE);
if (!ROUNDS_OF[GAME]) nothingFor(GAME, NONE);
const S = await import('data:text/javascript;base64,' + Buffer.from((await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/app/settings.ts')],
  bundle: true, format: 'esm', platform: 'neutral', write: false })).outputFiles[0].text).toString('base64'));
const V = await import('data:text/javascript;base64,' + Buffer.from((await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/app/vault.ts')],
  bundle: true, format: 'esm', platform: 'neutral', write: false })).outputFiles[0].text).toString('base64'));

const argv = process.argv.slice(2);
const opt = (k, d) => { const i = argv.indexOf('--' + k); return i >= 0 && i + 1 < argv.length ? argv[i + 1] : d; };
const BASE = opt('base'), BACKUP = opt('backup'), STATE = opt('state');
if (!BASE || !BACKUP) { console.error('usage: node web/tools/vault-pair.mjs --base <url> --backup <zip> [--state <dir>] [--headed]'); process.exit(2); }
await requireGame(BASE, GAME);

const fails = [];
const check = (ok, what) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) fails.push(what); };
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function until(what, f, ms = 20000) {
  const t0 = Date.now();
  for (;;) {
    const v = await f().catch(() => null);
    if (v) return v;
    if (Date.now() - t0 > ms) throw new Error(`${what}: not within ${ms / 1000} s`);
    await sleep(200);
  }
}

/** a page's file of the save folder: its SHA-256, or null */
const fileHash = (page, rel, save = 'save') => page.evaluate(async ([rel, save]) => {
  let d = await (await navigator.storage.getDirectory()).getDirectoryHandle(save);
  const parts = rel.split('/');
  for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p);
  const f = await (await d.getFileHandle(parts[parts.length - 1])).getFile();
  return [...new Uint8Array(await crypto.subtle.digest('SHA-256', await f.arrayBuffer()))].map(b => b.toString(16).padStart(2, '0')).join('');
}, [rel, save]).catch(() => null);
/** a page's file of the save folder as text, or null */
const readText = (page, rel) => page.evaluate(async rel => {
  let d = await (await navigator.storage.getDirectory()).getDirectoryHandle('save');
  const parts = rel.split('/');
  for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p);
  return (await (await d.getFileHandle(parts[parts.length - 1])).getFile()).text();
}, rel).catch(() => null);
/** a page's settings.ini as a keyphrase keeps it (settings.ts withoutDevice) */
const keptIni = async page => S.withoutDevice((await readText(page, 'settings.ini')) ?? '');
/** what a page's settings.ini sets these keys to */
const iniSets = async (page, ...keys) => { const r = S.resolveEnv(await readText(page, 'settings.ini'), {}, {});
  return keys.map(k => r.file[k]?.value ?? '').join(' '); };
/** a page's settings.ini with these keys set */
const putIni = async (page, sets) => writeFile(page, 'settings.ini',
  [...Buffer.from(sets.reduce((t, [k, v]) => S.settingsPut(t, k, v), (await readText(page, 'settings.ini')) ?? S.TEMPLATE))]);
const writeFile = (page, rel, bytes) => page.evaluate(async ({ rel, bytes }) => {
  let d = await (await navigator.storage.getDirectory()).getDirectoryHandle('save', { create: true });
  const parts = rel.split('/');
  for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p, { create: true });
  const w = await (await d.getFileHandle(parts[parts.length - 1], { create: true })).createWritable();
  await w.write(new Uint8Array(bytes)); await w.close();
}, { rel, bytes });
/** a page's DVD and magazine covers ("NFS Underground 2/<NAME> DVD 1/<NAME> DVD 1") */
const covers = page => page.evaluate(async () => {
  const out = [];
  try {
    const d = await (await (await navigator.storage.getDirectory()).getDirectoryHandle('save')).getDirectoryHandle('NFS Underground 2');
    for await (const [name, h] of d.entries()) if (h.kind === 'directory' && / (DVD|Magazine) [0-9]+$/.test(name)) out.push(name);
  } catch { /* none */ }
  return out.sort();
});
/** a page's files of the save folder that `want` takes (by path), with their bytes */
async function filesOf(page, want) {
  const paths = await page.evaluate(async () => {
    const out = [];
    async function walk(d, rel) {
      for await (const [n, h] of d.entries()) { const r = rel ? `${rel}/${n}` : n; if (h.kind === 'directory') await walk(h, r); else out.push(r); }
    }
    await walk(await (await navigator.storage.getDirectory()).getDirectoryHandle('save'), '');
    return out;
  });
  const out = [];
  for (const p of paths.filter(want)) out.push({ path: p, data: new Uint8Array(await page.evaluate(async rel => {
    let d = await (await navigator.storage.getDirectory()).getDirectoryHandle('save');
    const parts = rel.split('/');
    for (const x of parts.slice(0, -1)) d = await d.getDirectoryHandle(x);
    return [...new Uint8Array(await (await (await d.getFileHandle(parts[parts.length - 1])).getFile()).arrayBuffer())];
  }, p)) });
  return out;
}
const setLink = (page, l, save = 'save') => page.evaluate(([l, save]) => localStorage.setItem(`nfsu2-keyphrase:${save}`, JSON.stringify(l)), [l, save]);
const link = (page, save = 'save') => page.evaluate(save => JSON.parse(localStorage.getItem(`nfsu2-keyphrase:${save}`) ?? 'null'), save);
const phase = page => page.evaluate(() => window.__nfsu2?.phase);
const profiles = page => page.$$eval('#profiles .name', els => els.map(e => e.textContent));
const status = page => page.textContent('#vault-status');
/** synced: "Synced <when>" beside the heading, nothing to say under it */
const synced = async page => /^Synced /.test(await page.textContent('#vault-when')) && !(await status(page));
const syncedNote = page => page.textContent('#vault-when');
async function confirmOk(page) { await page.waitForSelector('#confirm[open]'); await page.click('#confirm-ok'); }
async function open(page, query = '') {
  await page.goto(`${BASE}/${query}`);
  await page.waitForSelector('#vault:not([hidden])', { timeout: 20000 });
  await sleep(500);                    // the tab takes the saves over (instance.ts), its settings load, it syncs
}
async function rename(page, from, to) {
  await page.click(`#profiles button[aria-label="Rename ${from}"]`);
  await page.fill('#profiles li.renaming input', to);
  await page.click('#profiles li.renaming [type="submit"]');
  await until(`${to} in the list`, async () => (await profiles(page)).includes(to));
}

const browser = await chromium.launch({ channel: 'chrome', headless: !argv.includes('--headed'), args: ['--enable-unsafe-webgpu'] });
try {
  // (A may write the clipboard: the eye copies the keyphrase; reading it back the page's own policy refuses)
  const a = await (await browser.newContext({ permissions: ['clipboard-write'] })).newPage(), b = await (await browser.newContext()).newPage();
  for (const [n, p] of [['A', a], ['B', b]]) p.on('pageerror', e => console.log(`${n} page error: ${e.message}`));

  // A: a backup's saves, then a keyphrase for them
  await open(a);
  await a.setInputFiles('#import', BACKUP);
  await confirmOk(a);
  const name = await until('a profile restored in A', async () => (await profiles(a))[0]);
  const at = n => `NFS Underground 2/${n}/${n}`;
  await putIni(a, [['render', 'scale:1.5'], ['fps', '120']]);           // A's own scale and frame rate
  await writeFile(a, 'gamefile.bin', [...crypto.randomBytes(300 << 10)]);   // the races A hosted: past the cap alone
  await open(a);
  await a.click('#vault-make');
  const phrase = await until('the keyphrase in the box', async () => (await link(a)) && (await a.inputValue('#vault-input')).trim());
  const box = page => page.evaluate(() => ({ masked: document.querySelector('#vault-box').classList.contains('masked'), dots: document.querySelector('#vault-mask').textContent,
    readOnly: document.querySelector('#vault-input').readOnly, selected: [document.querySelector('#vault-input').selectionStart, document.querySelector('#vault-input').selectionEnd] }));
  console.log(`A: profile ${name}, keyphrase ${phrase}`);
  const made = await box(a);
  check(/^nfsu2-xyz(-[a-z]{3,5}){8}$/.test(phrase) && !made.masked && made.readOnly, 'A made a keyphrase of nfsu2-xyz and 8 words, shown in the box, read only');
  await a.click('#vault-eye');
  const hid = await box(a);
  check(hid.masked && hid.dots === 'nfs' + '\u2022'.repeat(phrase.length - 3), `the eye hides it but its first 3 characters ("${hid.dots.slice(0, 6)}...")`);
  await a.click('#vault-eye');
  const shownAgain = await box(a);
  const copied = await until('Copied.', async () => (await a.textContent('#vault-msg')) === 'Copied.', 5000).catch(() => false);
  check(!shownAgain.masked && shownAgain.selected[0] === 0 && shownAgain.selected[1] === phrase.length && copied, 'shown again, it is all selected and copied');
  await a.click('#vault-eye');
  check(await synced(a), `A synced: "${await syncedNote(a)}"`);
  check(!!(await link(a))?.rev, 'A keeps the link in localStorage');

  // B: a screenshot of its own, then the keyphrase typed in another shape
  await open(b);
  await writeFile(b, 'nfsu2000.tga', [1, 2, 3, 4]);
  // and its own graphics options, resolution scale and frame rate
  await writeFile(b, 'registry.txt', [...Buffer.from('d g_RacingResolution=1\n')]);
  await putIni(b, [['render', 'scale:0.5'], ['fps', '30']]);
  const regB = await fileHash(b, 'registry.txt');
  await writeFile(b, 'gamefile.bin', [...crypto.randomBytes(2048)]);     // and the races B hosted
  const gameB = await fileHash(b, 'gamefile.bin');
  const typing = ` ${phrase.split('-').slice(2).join(' ').toUpperCase()} `;
  await b.fill('#vault-input', typing);
  const typed = await box(b);
  check(typed.masked && typed.dots === typing.slice(0, 3) + '\u2022'.repeat(typing.length - 3), 'what is typed is dotted too, but its first 3 characters');
  await b.click('#vault-load');
  await confirmOk(b);
  await until('the profile in B', async () => (await profiles(b)).includes(name));
  await until('B linked', async () => synced(b));
  check(true, `B loaded the keyphrase's saves: "${await syncedNote(b)}"`);
  check((await fileHash(a, at(name))) === (await fileHash(b, at(name))) && !!(await fileHash(b, at(name))), 'the profile is the same bytes in A and B');
  check((await keptIni(a)) === (await keptIni(b)), 'settings.ini came along, but its device settings');
  check(await iniSets(b, 'render', 'fps') === 'scale:0.5 30', `B kept its own resolution scale and frame rate (${await iniSets(b, 'render', 'fps')}; A: ${await iniSets(a, 'render', 'fps')})`);
  check((await fileHash(b, 'registry.txt')) === regB && regB !== (await fileHash(a, 'registry.txt')), 'B kept its own graphics options (registry.txt)');
  check((await fileHash(b, 'gamefile.bin')) === gameB && gameB !== (await fileHash(a, 'gamefile.bin')), 'B kept its own LAN server\'s file (gamefile.bin)');
  check(!!(await fileHash(b, 'nfsu2000.tga')), 'B\'s screenshot stayed');
  const coversA = await covers(a);
  check(!(await covers(b)).length, `A's covers stayed in A (${coversA.join(', ') || 'the backup has none'})`);

  // A changes its resolution scale and hosts a race: nothing is sent
  const revD = (await link(a)).rev;
  await putIni(a, [['render', 'scale:2']]);
  await writeFile(a, 'gamefile.bin', [...crypto.randomBytes(300 << 10)]);
  await open(a);
  await sleep(5000);
  check((await link(a))?.rev === revD && await synced(a), 'A\'s resolution scale and gamefile.bin changed: nothing sent');

  // A changes settings.ini: B takes it, and keeps a cover of its own of the profile and its own scale
  const own = `${name} DVD 9`, ownAt = `NFS Underground 2/${own}/${own}`;
  await writeFile(b, ownAt, [...crypto.randomBytes(4096)]);
  const revS = (await link(a)).rev;
  await putIni(a, [['horn', 'trombone']]);
  await open(a);
  await until('A sent settings.ini', async () => (await link(a))?.rev !== revS);
  await open(b);
  await until('settings.ini in B', async () => (await keptIni(a)) === (await keptIni(b)));
  check(!!(await fileHash(b, ownAt)) && await iniSets(b, 'horn', 'render') === 'trombone scale:0.5',
    `B took A's settings.ini and kept its own cover (${own}) and scale (${await iniSets(b, 'horn', 'render')})`);

  // A renames: B has it after its next page load
  const rev0 = (await link(a)).rev;
  await rename(a, name, 'RENAMED');
  await until('A sent the rename', async () => (await link(a))?.rev !== rev0);
  await open(b);
  await until('the rename in B', async () => (await profiles(b)).includes('RENAMED'));
  check(!(await profiles(b)).includes(name), 'B took A\'s rename');
  check(!!(await fileHash(b, 'nfsu2000.tga')), 'B\'s screenshot stayed after the sync');

  // A's send kept by the server but its answer lost (issue 33): its next send is no conflict
  await a.route('**/vault', async r => { if (r.request().method() !== 'PUT') return r.continue(); await r.fetch(); await r.abort('failed'); });
  await rename(a, 'RENAMED', 'LOSTA');
  await until('A did not hear back', async () => /^Sync failed/.test(await status(a)), 30000);
  await a.unroute('**/vault');
  await rename(a, 'LOSTA', 'RENAMED');
  await until('A synced or not', async () => (await synced(a)) || /Keep which/.test(await status(a)), 30000);
  check(await synced(a) && !(await link(a)).sent, `A's own unanswered send is no conflict: "${await status(a)}"`);
  await open(b);
  await until('B took it', async () => (await profiles(b)).includes('RENAMED') && await synced(b));

  // both rename: a conflict in B, both sides shown, which keeps its own when asked; A takes it at its next load
  const rev1 = (await link(a)).rev;
  await rename(a, 'RENAMED', 'FROMA');
  await until('A sent its rename', async () => (await link(a))?.rev !== rev1);
  await rename(b, 'RENAMED', 'FROMB');
  await b.waitForSelector('#vault-conflict:not([hidden])', { timeout: 20000 });
  check(/^Changed here and in another browser/.test(await status(b)), `B sees the conflict: "${await status(b)}"`);
  const sides = await b.locator('#vault-sides').innerText();
  check(/This browser[\s\S]*FROMB \$[\s\S]*Cloud[\s\S]*FROMA \$/.test(sides), `B shows both sides: ${JSON.stringify(sides)}`);
  await b.click('#vault-mine');
  await confirmOk(b);
  await until('B synced again', async () => synced(b));
  await open(a);
  await until('B\'s rename in A', async () => (await profiles(a)).includes('FROMB'));
  check(!(await profiles(a)).includes('FROMA'), 'A took B\'s choice');

  // A as the page before 2026-10-28 linked it: version 3, its hash over gamefile.bin too
  const v3 = async () => V.contentHash(V.kept(await filesOf(a, V.keptBy(3)), 3));
  const l3 = { ...(await link(a)), v: 3, hash: await v3() };
  await setLink(a, l3);
  await open(a);
  await sleep(3000);
  const up = await link(a);
  check(up.v === 4 && up.rev === l3.rev && up.hash === await V.contentHash(V.kept(await filesOf(a, V.keeps))) && await synced(a),
    `a version 3 link, the files unchanged: version 4, the hash of now, nothing sent (v ${up.v}, "${await syncedNote(a)}")`);
  // the same, but A hosted races since (300 KB more): it sends at its next page load, without gamefile.bin (the page of
  // then failed past the cap there), and a rename after that goes too
  const rev3 = (await link(a)).rev;
  await setLink(a, { ...(await link(a)), v: 3, hash: await v3() });
  await writeFile(a, 'gamefile.bin', [...crypto.randomBytes(300 << 10)]);
  await open(a);
  await until('A sent again', async () => (await link(a))?.rev !== rev3 && await synced(a), 30000);
  check((await link(a)).v === 4, `a version 3 link whose gamefile.bin grew past the cap sends again: "${await syncedNote(a)}"`);
  const rev4 = (await link(a)).rev;
  await rename(a, 'FROMB', 'STUCK');
  await until('A sent the rename', async () => (await link(a))?.rev !== rev4 && await synced(a), 30000);
  await open(b);
  await until('the rename in B', async () => (await profiles(b)).includes('STUCK') && await synced(b));
  check((await fileHash(b, 'gamefile.bin')) === gameB, 'B took it and kept its own gamefile.bin');

  if (STATE) {
    // the keyphrase's file: the SHA-256 of its token (vault.ts keysOf), one file for both versions
    const token = crypto.pbkdf2Sync(phrase, 'nfsu2-xyz keyphrase saves v1', 100000, 64, 'sha256').subarray(32).toString('base64url');
    const id = crypto.createHash('sha256').update(token).digest('hex');
    const shard = path.join(STATE, 'vault', id.slice(0, 3));
    const files = fs.readdirSync(shard).filter(f => f.startsWith(id));
    check(files.length === 1 && files[0] === id, `its file on the server: ${id.slice(0, 3)}/${files.join(', ')}`);
    const held = files.length ? fs.readFileSync(path.join(shard, files[0])) : Buffer.alloc(0);
    check(held.subarray(0, 4).toString() === 'NFV1' && !held.includes('STUCK') && !held.includes('20CM') && !held.includes('settings'), 'it holds no name, no profile, no settings in the clear');
    check(!held.includes(phrase.split('-')[2]), 'nor the keyphrase');
  }

  // A deletes the saves from the server: B is told at its next load, and keeps its own
  await a.click('#vault-delete');
  await confirmOk(a);
  await until('A unlinked', async () => !(await link(a)));
  await open(b);
  await until('B told', async () => /no saves for this keyphrase/.test(await status(b)));
  check(true, `B: "${await status(b)}"`);
  check((await profiles(b)).includes('STUCK') && !!(await fileHash(b, 'nfsu2000.tga')), 'B keeps its saves and screenshot');

  if (argv.includes('--play')) {
    const ENV = ['NOMOVIES=1', 'SNDSYNC=0', 'GPUVS=1', 'NFSU2_TIMEOUT=400'].map(e => `env=${encodeURIComponent(e)}`).join('&');
    const c = await (await browser.newContext()).newPage(), d = await (await browser.newContext()).newPage();
    c.on('pageerror', e => console.log(`C page error: ${e.message}`));
    await open(c, `?save=vplay&fresh&${ENV}&script=/tests/scripts/app_rw_probe.txt`);
    await until('C ready to play', async () => (await phase(c)) === 'ready', 120000);
    await c.click('#vault-make');
    const phrase2 = await until('C\'s keyphrase', async () => (await link(c, 'vplay')) && (await c.inputValue('#vault-input')).trim());
    check((await link(c, 'vplay'))?.profiles === 0, 'C made a keyphrase with no profile yet');
    await c.click('#play');
    await until('C playing', async () => (await phase(c)) === 'playing', 60000);
    let during = false;
    await until('the game over', async () => {
      const [ph, l] = [await phase(c), await link(c, 'vplay')];
      if (ph === 'playing' && l?.profiles === 1) during = true;
      return ph === 'exited';
    }, 400000);
    const exit = await c.evaluate(() => window.__nfsu2.lastExit);
    const log = await c.evaluate(() => window.__nfsu2.log.text());
    check(exit?.code === 0 && /the game saved profile ROTEST/.test(log), `C's game saved ROTEST and closed (${JSON.stringify(exit)})`);
    check(during, 'its save went to the server while the game ran');
    await until('C synced after the game', async () => (await synced(c)) && (await link(c, 'vplay'))?.profiles === 1);
    check(true, `C after the game: "${await syncedNote(c)}"`);
    await open(d, '?save=vplay2&fresh');
    await d.fill('#vault-input', phrase2);
    await d.click('#vault-load');
    await confirmOk(d);
    await until('ROTEST in D', async () => (await profiles(d)).includes('ROTEST'));
    check((await fileHash(c, at('ROTEST'), 'vplay')) === (await fileHash(d, at('ROTEST'), 'vplay2')), 'D has the game\'s save, the same bytes');
    const rev2 = (await link(d, 'vplay2')).rev;
    await rename(d, 'ROTEST', 'AFTER');
    await until('D sent the rename', async () => (await link(d, 'vplay2'))?.rev !== rev2);
    // C, not reloaded: Play takes the rename before the game starts
    await c.click('#play');
    await until('C playing again', async () => (await phase(c)) === 'playing', 60000);
    const took = !!(await fileHash(c, at('AFTER'), 'vplay')) && !(await fileHash(c, at('ROTEST'), 'vplay'));
    if (!took) console.log(`C: "${await status(c)}"\n${(await c.evaluate(() => window.__nfsu2.log.text())).split('\n').filter(l => /keyphrase|\[app\] (the game|autosave|settings)/.test(l)).slice(-12).join('\n')}`);
    // F9, Quit game: the game's own close (or its input script's, which ends with one)
    await sleep(5000);
    await c.keyboard.press('F9');
    await c.click('#menu [data-act="quit"]').catch(() => {});
    await until('C quit', async () => (await phase(c)) === 'exited', 400000);
    check(took, 'C\'s Play took D\'s rename before the game started');
  }
} catch (e) {
  check(false, `stopped: ${e.message}`);
} finally {
  await browser.close();
}
console.log(fails.length ? `FAIL (${fails.length})` : 'PASS');
process.exit(fails.length ? 1 : 0);
