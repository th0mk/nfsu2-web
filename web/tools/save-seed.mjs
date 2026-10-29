// A save tree (NFSU2_SAVE_SEED, a player's "Download a backup" unzipped, the
// Mac port's work/seed/...) written into a page's OPFS save directory, from a
// page of the server's origin; the test page then runs without &fresh.  Its
// empty folders go in too (run-suite.mjs --save-out keeps them): a folder a
// delete left behind is part of the state the next run starts from.
// Used by run-suite.mjs and roam-pair.mjs (--seed).
import fs from 'node:fs';
import path from 'node:path';

export async function seedSave(page, base, dir, tree) {
  const files = [], empty = [];
  (function walk(d, rel) {
    const es = fs.readdirSync(d, { withFileTypes: true });
    if (!es.length && rel) empty.push(rel);
    for (const e of es) {
      const r = rel ? `${rel}/${e.name}` : e.name;
      if (e.isDirectory()) walk(path.join(d, e.name), r);
      else if (e.isFile()) files.push([r, fs.readFileSync(path.join(d, e.name)).toString('base64')]);
    }
  })(tree, '');
  await page.goto(`${base}/players`);
  return page.evaluate(async ([dir, files, empty]) => {
    const root = await navigator.storage.getDirectory();
    try { await root.removeEntry(dir, { recursive: true }); } catch { /* none yet */ }
    const top = await root.getDirectoryHandle(dir, { create: true });
    for (const [rel, b64] of files) {
      const parts = rel.split('/'); let d = top;
      for (const p of parts.slice(0, -1)) d = await d.getDirectoryHandle(p, { create: true });
      const w = await (await d.getFileHandle(parts.at(-1), { create: true })).createWritable();
      await w.write(Uint8Array.from(atob(b64), c => c.charCodeAt(0))); await w.close();
    }
    for (const rel of empty) { let d = top; for (const p of rel.split('/')) d = await d.getDirectoryHandle(p, { create: true }); }
    return files.length;
  }, [dir, files, empty]);
}
