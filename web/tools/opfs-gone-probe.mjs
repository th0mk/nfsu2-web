// What a worker's OPFS handles answer once their folder is gone, Firefox beside Chrome (review of 3dbf65a0; io-worker.ts
// saveGone/recover/saveTrouble).  The folder goes by removeEntry(recursive) from the page (any browser), or in Chrome
// by Storage.clearDataForOrigin.  Found 2026-11-02 (Firefox 157, Chrome 156): keys() answers NotFoundError in both, but a
// create (getFileHandle/getDirectoryHandle with create) in a removed folder answers UnknownError in Firefox, NotFoundError
// in Chrome; a held file's folder cannot be removed in either (NoModificationAllowedError).  ('csd': a Clear-Site-Data
// response made by page.route: it clears nothing in either browser.)
//   node web/tools/opfs-gone-probe.mjs <base> [remove|clear|csd]
import { chromium, firefox } from 'playwright';
const base = process.argv[2] ?? 'http://127.0.0.1:8696';
const worker = `
  let d, sh, fh;
  onmessage = async e => {
    const out = [], tryit = async (what, f) => { try { const r = await f(); out.push(what + ': ok' + (r === undefined ? '' : ' ' + r)); } catch (x) { out.push(what + ': ' + x.name); } };
    if (e.data === 'open') {
      const root = await navigator.storage.getDirectory();
      d = await root.getDirectoryHandle('rr-probe', { create: true });
      const sub = await d.getDirectoryHandle('NFS Underground 2', { create: true });
      fh = await sub.getFileHandle('TOP', { create: true });
      sh = await fh.createSyncAccessHandle({ mode: 'readwrite-unsafe' }).catch(async () => fh.createSyncAccessHandle());
      sh.truncate(0); sh.write(new Uint8Array(1000).fill(7), { at: 0 }); sh.flush();
      if (e.data === 'open') { sh.close(); sh = null; }
      postMessage('opened');
    } else if (e.data === 'open-held') {
      const root = await navigator.storage.getDirectory();
      d = await root.getDirectoryHandle('rr-probe', { create: true });
      const sub = await d.getDirectoryHandle('NFS Underground 2', { create: true });
      fh = await sub.getFileHandle('TOP', { create: true });
      sh = await fh.createSyncAccessHandle({ mode: 'readwrite-unsafe' }).catch(async () => fh.createSyncAccessHandle());
      sh.truncate(0); sh.write(new Uint8Array(1000).fill(7), { at: 0 }); sh.flush();
      postMessage('opened, held');
    } else if (e.data === 'after') {
      await tryit('old folder keys()', async () => { for await (const k of d.keys()) break; });
      await tryit('old folder getFileHandle(new, create)', () => d.getFileHandle('settings.ini', { create: true }).then(() => {}));
      await tryit('old folder getDirectoryHandle(sub)', () => d.getDirectoryHandle('NFS Underground 2').then(() => {}));
      await tryit('old folder getDirectoryHandle(new, create) [the game\\'s CreateDirectoryA]', () => d.getDirectoryHandle('NEWDIR', { create: true }).then(() => {}));
      await tryit('old file createSyncAccessHandle', async () => { const h = await fh.createSyncAccessHandle(); h.close(); });
      if (sh) {
        await tryit('held handle write', () => sh.write(new Uint8Array(10).fill(9), { at: 1000 }));
        await tryit('held handle getSize', () => sh.getSize());
        await tryit('held handle read 1000', () => { const b = new Uint8Array(1000); return sh.read(b, { at: 0 }) + ' bytes, first ' + b[0]; });
        await tryit('held handle close', () => sh.close());
      }
      const root = await navigator.storage.getDirectory();
      await tryit('the folder made again', () => root.getDirectoryHandle('rr-probe', { create: true }).then(() => {}));
      postMessage(out.join('\\n'));
    }
  };`;
async function run(name, type, how, held) {
  const browser = await type.launch({ headless: true, ...(type === chromium ? { channel: 'chrome' } : {}) });
  const ctx = await browser.newContext({ bypassCSP: true }), page = await ctx.newPage();
  await page.goto(`${base}/robots.txt`).catch(() => {});
  await page.evaluate(src => { window.w = new Worker(URL.createObjectURL(new Blob([src], { type: 'text/javascript' }))); window.msgs = []; window.w.onmessage = e => window.msgs.push(e.data); window.w.onerror = e => window.msgs.push('worker error ' + e.message); }, worker);
  await page.evaluate(h => window.w.postMessage(h ? 'open-held' : 'open'), held);
  await page.waitForFunction(() => window.msgs.length >= 1, null, { timeout: 15000 });
  let gone = '';
  if (how === 'clear') { const cdp = await ctx.newCDPSession(page); await cdp.send('Storage.clearDataForOrigin', { origin: base, storageTypes: 'all' }); gone = 'Storage.clearDataForOrigin'; }
  else if (how === 'csd') {
    // the browser's own clearing: a response of the origin saying Clear-Site-Data: "storage"
    await page.route('**/rr-clear', r => r.fulfill({ status: 200, headers: { 'Clear-Site-Data': '"storage"', 'content-type': 'text/plain' }, body: 'cleared' }));
    await page.evaluate(async () => { await fetch('/rr-clear', { cache: 'no-store' }).then(r => r.text()); });
    await page.waitForTimeout(1500);
    const left = await page.evaluate(async () => { const n = []; for await (const k of (await navigator.storage.getDirectory()).keys()) n.push(k); return n.join(',') || 'nothing'; });
    gone = `Clear-Site-Data "storage" (the origin's OPFS has now: ${left})`;
  }
  else gone = await page.evaluate(async () => { try { await (await navigator.storage.getDirectory()).removeEntry('rr-probe', { recursive: true }); return 'removeEntry: removed'; } catch (e) { return 'removeEntry: ' + e.name; } });
  await page.evaluate(() => window.w.postMessage('after'));
  await page.waitForFunction(() => window.msgs.length >= 2, null, { timeout: 15000 });
  console.log(`\n== ${name}, the folder's file ${held ? 'held open by a sync handle' : 'closed'}; ${gone}\n${(await page.evaluate(() => window.msgs[1])).split('\n').map(s => '   ' + s).join('\n')}`);
  await browser.close();
}
const only = process.argv[3];
for (const held of [false, true]) {
  if (!only || only === 'remove') {
    await run('Firefox (Playwright 157)', firefox, 'remove', held).catch(e => console.log('firefox failed', e.message));
    await run('Chrome', chromium, 'remove', held).catch(e => console.log('chrome failed', e.message));
  }
  if (!only || only === 'csd') {
    await run('Firefox (Playwright 157)', firefox, 'csd', held).catch(e => console.log('firefox csd failed', e.message));
    await run('Chrome', chromium, 'csd', held).catch(e => console.log('chrome csd failed', e.message));
  }
  if (!only || only === 'clear') await run('Chrome', chromium, 'clear', held).catch(e => console.log('chrome clear failed', e.message));
}
