// The page's own save-folder code (savefs.ts checkSaves, listFiles, readText, writeFile) in a real browser's OPFS, with
// the locks a real browser holds: a worker's sync access handle, as the game's I/O worker or another tab has one.  A
// player's Firefox 159 (Discord 2026-11-04) answered every call in its save folder with "UnknownError: Unknown failure";
// that state cannot be made through the API, so this checks the parts a browser can show: no healthy folder fails the
// check (a game's handles on its files included), nor a test file an earlier check left that another context holds (the
// check writes under a new name each try, and removes its own leftovers once they can go, never a name that only starts
// like theirs), a write refused over a held file leaves it, and nothing is left beside the folder.  Serves its own page,
// no game server.
//   node web/tools/opfs-trouble-probe.mjs [firefox|chromium]   (Playwright's browser of that name; Firefox headless is fine)
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { chromium, firefox } from 'playwright';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const which = process.argv[2] ?? 'firefox';
const js = (await esbuild.build({ stdin: { contents: `export * from './web/src/app/savefs.ts';`, resolveDir: ROOT, loader: 'ts' },
  bundle: true, format: 'iife', globalName: 'SF', write: false, logLevel: 'error' })).outputFiles[0].text;
// a worker that holds a sync access handle on a file of the OPFS root's path (as the game's I/O worker does) until told
const worker = `let sh;
onmessage = async e => {
  if (e.data.hold) {
    let d = await navigator.storage.getDirectory();
    const p = e.data.hold.split('/');
    for (const x of p.slice(0, -1)) d = await d.getDirectoryHandle(x, { create: true });
    sh = await (await d.getFileHandle(p.at(-1), { create: true })).createSyncAccessHandle();
    postMessage('held');
  } else { sh?.close(); sh = null; postMessage('let go'); }
};`;
const page = `<!doctype html><meta charset="utf-8"><title>opfs trouble probe</title><script src="/savefs.js"></script><script>
const S = 'probe-save', enc = new TextEncoder();
const w = new Worker('/hold.js');
const ask = m => new Promise(r => { w.onmessage = e => r(e.data); w.postMessage(m); });
const err = e => e ? e.name + ': ' + e.message : 'none';
const besides = async () => { const out = []; for await (const n of (await navigator.storage.getDirectory()).keys()) out.push(n); return out.sort().join(','); };
window.run = async () => {
  const out = {}, root = await navigator.storage.getDirectory();
  for await (const n of root.keys()) await root.removeEntry(n, { recursive: true });
  await SF.writeFile(S, 'NFS Underground 2/TOP/TOP', new Uint8Array(54966).fill(3));
  await SF.writeFile(S, 'settings.ini', '# mine\\nhorn = train\\n');
  await SF.writeFile(S, 'registry.txt', 'd PowerUnits=1\\n');
  out.healthy = await SF.checkSaves(S);
  out.left = await besides();
  out.list = (await SF.listFiles(S)).map(f => f.path + ' ' + f.size).join('; ');
  // the game's own handles on its files (Firefox: one exclusive handle a file)
  await ask({ hold: S + '/settings.ini' });
  out.heldSettings = await SF.checkSaves(S);
  out.readHeld = await SF.readText(S, 'settings.ini');
  out.writeOverHeld = await SF.writeFile(S, 'settings.ini', '# the template\\n').then(() => 'written', err);
  out.afterRefusedWrite = await SF.readText(S, 'settings.ini');
  await ask({});
  // a test file an earlier check left (the fixed name before the fresh ones), held by another context: the check writes
  // beside it under its own name, and leaves it while held; let go, the next check removes it
  await ask({ hold: S + '~check' });
  const held = await SF.checkSaves(S, [50, 100, 200]);
  out.heldLeftover = { ok: held.ok, tries: held.tries, step: held.step, error: err(held.error) };
  out.leftWhileHeld = await besides();
  await ask({});
  out.letGo = await SF.checkSaves(S);
  out.leftAfter = await besides();
  // a fresh-name leftover goes too; a name that only starts like theirs stays
  for (const n of [S + '~check-old1', S + '~checkers']) { const w = await (await root.getFileHandle(n, { create: true })).createWritable(); await w.write('x'); await w.close(); }
  out.others = await SF.checkSaves(S);
  out.leftOthers = await besides();
  await root.removeEntry(S + '~checkers');
  out.missing = { text: await SF.readText('no-such-folder', 'settings.ini'), exists: await SF.exists(S, 'nothing.txt'), list: (await SF.listFiles('no-such-folder')).length, folders: await besides() };
  for await (const n of root.keys()) await root.removeEntry(n, { recursive: true });
  return out;
};
</script>`;
const server = http.createServer((q, r) => {
  const [type, body] = q.url === '/savefs.js' ? ['text/javascript', js] : q.url === '/hold.js' ? ['text/javascript', worker] : ['text/html', page];
  r.writeHead(200, { 'content-type': type }); r.end(body);
});
await new Promise(r => server.listen(0, '127.0.0.1', r));
const browser = await (which === 'chromium' ? chromium : firefox).launch({ headless: true });
let out;
try {
  const p = await browser.newPage();
  await p.goto(`http://127.0.0.1:${server.address().port}/`);
  out = await p.evaluate(() => window.run());
  console.log(`${which} ${browser.version()}`);
  console.log(JSON.stringify(out, null, 2));
} finally { await browser.close(); server.close(); }
const checks = [
  ['a healthy folder passes at once', out.healthy.ok && out.healthy.tries === 1],
  ['nothing is left beside the save folder', out.left === 'probe-save' && out.leftAfter === 'probe-save'],
  ['a held leftover does not fail the check, and stays while held', out.heldLeftover.ok && out.heldLeftover.tries === 1 && out.leftWhileHeld === 'probe-save,probe-save~check'],
  ['once let go, the next check removes it', out.letGo.ok && out.leftAfter === 'probe-save'],
  ['its own fresh-name leftovers go, a name that only starts like theirs stays', out.others.ok && out.leftOthers === 'probe-save,probe-save~checkers'],
  ['the game\'s handle on a file of the folder does not fail the check', out.heldSettings.ok && out.heldSettings.tries === 1],
  ['a held file reads', out.readHeld === '# mine\nhorn = train\n'],
  ['a write refused over a held file leaves it', out.writeOverHeld !== 'written' && out.afterRefusedWrite === '# mine\nhorn = train\n'],
  ['what is not there reads as none, and makes no folder', out.missing.text === null && out.missing.exists === false && out.missing.list === 0 && out.missing.folders === 'probe-save'],
];
for (const [what, ok] of checks) console.log(`${ok ? 'PASS' : 'FAIL'}: ${what}`);
process.exitCode = checks.every(c => c[1]) ? 0 : 1;
