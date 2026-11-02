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
const { memoryTries, allocMemory } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

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
