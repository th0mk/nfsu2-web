// The start page's storage check (web/src/app/checks.ts checkStorage): which browsers can keep the game's files.
//   node --test web/test/checks-storage.test.mjs
// A fake navigator.storage per case, and FileSystemFileHandle with or without createWritable (Safari before 26).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const out = await esbuild.build({ entryPoints: [path.join(HERE, '../src/app/checks.ts')], bundle: true, write: false, format: 'esm', platform: 'neutral' });
const C = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));

const IOS18 = 'Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.6 Mobile/15E148 Safari/604.1';
const CHROME = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36';

/** the storage check of one browser; opfs: it has an origin private file system here; writable: its file handles
 *  have createWritable (handle: false, no FileSystemFileHandle at all) */
function storage({ ua = CHROME, opfs = true, writable = true, handle = true, persisted = true }) {
  Object.defineProperty(globalThis, 'navigator', { configurable: true, writable: true, value: {
    userAgent: ua, maxTouchPoints: 0,
    storage: {
      getDirectory: async () => { if (!opfs) throw new DOMException('no', 'SecurityError'); return {}; },
      persisted: async () => persisted, persist: async () => persisted,
    },
  } });
  if (handle) globalThis.FileSystemFileHandle = class { getFile() {} };
  else delete globalThis.FileSystemFileHandle;
  if (handle && writable) globalThis.FileSystemFileHandle.prototype.createWritable = function () {};
  return C.checkStorage();
}

test('a browser that writes files from the page: no problem', async () => {
  const c = await storage({});
  assert.equal(c.state, 'ok');
  assert.equal(c.code, undefined);
});

test('Safari before 26 (no createWritable): Play stays off, with STORAGE-OLD', async () => {
  const c = await storage({ ua: IOS18, writable: false });
  assert.equal(c.state, 'fail');
  assert.equal(c.code, 'STORAGE-OLD');
  assert.match(c.detail, /iOS 26/);
});

test('no FileSystemFileHandle at all: STORAGE-OLD too', async () => {
  const c = await storage({ handle: false });
  assert.equal(c.state, 'fail');
  assert.equal(c.code, 'STORAGE-OLD');
});

test('a window that stores nothing says so first (STORAGE-NONE), old or not', async () => {
  const c = await storage({ ua: IOS18, opfs: false, writable: false });
  assert.equal(c.code, 'STORAGE-NONE');
});
