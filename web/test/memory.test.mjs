// The game's shared memory (web/src/host/memory.ts): the build's size growing to 4 GB where the browser gives it,
// else the same size not growing; and a log line saying which.  A browser that refuses is played by a constructor
// that throws RangeError, as V8 and Safari do.
//   node --test web/test/memory.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const js = (await esbuild.build({ entryPoints: [path.join(ROOT, 'web/src/host/memory.ts')], bundle: true, format: 'esm', write: false })).outputFiles[0].text;
const { memoryTries, allocMemory, memoryOffPage, MemoryBuffer } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

const GB = 16384;                       // pages: the build's size (game.json "pages")
const tries = memoryTries(GB);
const REFUSED = 'WebAssembly.Memory(): could not allocate memory';
// refuses a memory whose maximum it can't set aside (Safari with no 4 GB piece left in its area)
const browser = reserve => {
  const asked = [];
  const make = t => {
    asked.push(t);
    if (t.maximum > reserve) throw new RangeError(REFUSED);
    return { t };
  };
  return { asked, make };
};

test('the build\'s size growing to 4 GB, then the same size not growing; nothing smaller', () => {
  assert.deepEqual(tries, [{ initial: GB, maximum: 65536 }, { initial: GB, maximum: GB }]);
});

test('a browser that gives the build\'s size: as before, one try, and the log says so', () => {
  const b = browser(65536);
  const { memory, note } = allocMemory(tries, b.make);
  assert.deepEqual(memory.t, { initial: GB, maximum: 65536 });
  assert.equal(b.asked.length, 1);
  assert.equal(note, '[memory] 1024 MB, up to 4096 MB');
});

test('Safari with no 4 GB piece left: the same size that does not grow', () => {
  const b = browser(GB);
  const { memory, note } = allocMemory(tries, b.make);
  assert.deepEqual(memory.t, { initial: GB, maximum: GB });
  assert.equal(note, `[memory] 1024 MB, not growing: the browser refused 1024 MB, up to 4096 MB (${REFUSED})`);
});

test('nothing given: the last refusal, for the page\'s GAME-START, with every try for the log', () => {
  const b = browser(0);
  assert.throws(() => allocMemory(tries, b.make), e => e instanceof RangeError
    && e.note === `[memory] the browser refused every size: 1024 MB, up to 4096 MB (${REFUSED}), then 1024 MB, not growing (${REFUSED})`);
  assert.equal(b.asked.length, tries.length);
});

test('a computer: 2 GB first for the guest heap\'s window, then the tries of before; elsewhere those alone', () => {
  assert.deepEqual(memoryTries(GB, true), [{ initial: 32768, maximum: 65536, optional: true }, ...tries]);
  assert.deepEqual(memoryTries(GB, false), tries);
  const b = browser(65536);
  const { memory, note } = allocMemory(memoryTries(GB, true), b.make);
  assert.deepEqual(memory.t, { initial: 32768, maximum: 65536, optional: true });
  assert.equal(note, '[memory] 2048 MB, up to 4096 MB');
  // a browser that can't give 2 GB from the start: today's 1 GB, and the log says what it refused
  const small = { asked: [], make: t => { small.asked.push(t); if (t.initial > GB) throw new RangeError(REFUSED); return { t }; } };
  const r = allocMemory(memoryTries(GB, true), small.make);
  assert.deepEqual(r.memory.t, { initial: GB, maximum: 65536 });
  assert.equal(r.note, `[memory] 1024 MB, up to 4096 MB: the browser refused 2048 MB, up to 4096 MB (${REFUSED})`);
  // Firefox's refusal of a memory it can't commit is a plain "out of memory", not a RangeError: passed over for the
  // window's try only
  const ff = t => { if (t.initial > GB) throw 'out of memory'; return { t }; };
  const f = allocMemory(memoryTries(GB, true), ff);
  assert.deepEqual(f.memory.t, { initial: GB, maximum: 65536 });
  assert.equal(f.note, '[memory] 1024 MB, up to 4096 MB: the browser refused 2048 MB, up to 4096 MB (out of memory)');
  assert.throws(() => allocMemory(tries, () => { throw 'out of memory'; }), e => e === 'out of memory');
  assert.throws(() => allocMemory(memoryTries(GB, true), () => { throw new TypeError('no shared memory'); }), TypeError);
});

test('no shared memory at all is no size question: thrown at once', () => {
  let n = 0;
  assert.throws(() => allocMemory(tries, () => { n++; throw new TypeError('shared memory is not available'); }), TypeError);
  assert.equal(n, 1);
});

test('the real constructor makes a shared memory of the size tried', () => {
  const { memory } = allocMemory(memoryTries(32));
  assert.ok(memory.buffer instanceof SharedArrayBuffer);
  assert.equal(memory.buffer.byteLength, 32 * 65536);
});

// WebKit's page holds the memory's buffer only (memory.ts: a live WebAssembly.Memory keeps JavaScriptCore from collecting)
test('the page holds the buffer only in WebKit: Safari and every browser on an iPhone, not Chrome or Firefox', () => {
  const safari = 'Mozilla/5.0 (iPhone; CPU iPhone OS 27_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/27.0 Mobile/15E148 Safari/604.1';
  const crios = 'Mozilla/5.0 (iPhone; CPU iPhone OS 27_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) CriOS/150.0 Mobile/15E148 Safari/604.1';
  const macSafari = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/27.0 Safari/605.1.15';
  const chrome = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36';
  const firefox = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10.15; rv:159.0) Gecko/20100101 Firefox/159.0';
  assert.deepEqual([safari, crios, macSafari, chrome, firefox].map(u => memoryOffPage(u)), [true, true, true, false, false]);
});

test('the buffer as the memory grows: a longer one is taken, a wait ends with it or after its time', async () => {
  const mem = new WebAssembly.Memory({ initial: 1, maximum: 4, shared: true });
  const view = new MemoryBuffer(mem.buffer);
  assert.equal(await view.until(65536), true);
  const waited = view.until(3 * 65536, 2000), late = view.until(5 * 65536, 50);
  assert.equal(view.grew(mem.buffer), false);
  mem.grow(2);
  assert.equal(view.grew(mem.buffer), true);
  assert.equal(view.buffer.byteLength, 3 * 65536);
  assert.equal(view.grew(new SharedArrayBuffer(65536)), false);
  assert.equal(await waited, true);
  assert.equal(await late, false);
});
