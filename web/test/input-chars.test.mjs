// The autotest's ns* keys on a page without the input layer (the suite's
// headless page): runtime/web/input_web.c translates them in C, and must
// write the same key-down records as the page's own path (synth.ts's
// synthetic KeyboardEvent through index.ts: keymap.ts keyFor, keyChars,
// keyVk), US layout, macOS rules.
//   node --test web/test/input-chars.test.mjs        (needs wasi-sdk in deps/)
// web/test/input_unit.c prints the C driver's record for every key, Caps Lock
// off and on, and every Shift/Ctrl/Alt combination (plus Cmd); this test
// computes the page's record for the same keystroke and compares all of them
// (the review of corefix found Option+Shift on a non-letter typed text on the
// page and was a command in C).
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const WASI = fs.readdirSync(path.join(REPO, 'deps')).filter(d => d.startsWith('wasi-sdk-')).map(d => path.join(REPO, 'deps', d)).pop();
const CC = WASI && path.join(WASI, 'bin/clang');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
// web/build.sh's target, features and defines
const FLAGS = ['--target=wasm32-wasip1-threads', '-pthread', '-O2', '-msimd128', '-mbulk-memory', '-mnontrapping-fptoint',
  '-mtail-call', '-mexception-handling', '-mllvm', '-wasm-enable-sjlj', '-mllvm', '-wasm-use-legacy-eh=false',
  '-D_WASI_EMULATED_MMAN', '-D_WASI_EMULATED_SIGNAL', '-D_WASI_EMULATED_PROCESS_CLOCKS', '-fno-delete-null-pointer-checks',
  '-include', 'runtime/web/compat/wasm_plat.h', '-Iruntime/web/compat', '-Iruntime', '-Iruntime/web'];
const LINK = ['-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=268435456',
  '-Wl,-z,stack-size=1048576', '-lsetjmp', '-lwasi-emulated-mman', '-lwasi-emulated-signal', '-lwasi-emulated-process-clocks'];

// web/src/input/proto.ts
const ISYN_SHIFT = 1, ISYN_CTRL = 2, ISYN_ALT = 4, ISYN_META = 8;
const IKF_ALT = 2, IKF_CTRL = 4, IKF_SHIFT = 8, IKF_DEAD = 16, IKF_SYNTH = 32;

async function keymap() {
  const out = await esbuild.build({ entryPoints: [path.join(REPO, 'web/src/input/keymap.ts')], bundle: true, format: 'esm',
    write: false, platform: 'neutral', logLevel: 'silent' });
  return import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));
}

/** index.ts onKeyDown for synth.ts's event: the key-down record, or null (none pushed) */
function pageRecord(m, dik, mods, caps) {
  const code = m.CODE_OF_DIK[dik];
  const shift = !!(mods & ISYN_SHIFT), ctrl = !!(mods & ISYN_CTRL), alt = !!(mods & ISYN_ALT), meta = !!(mods & ISYN_META);
  if (meta) return null;                                   // the browser's shortcut
  const e = { key: m.keyFor(code, shift, !!caps, true, null), ctrlKey: ctrl, altKey: alt, metaKey: meta,
    getModifierState: k => k === 'CapsLock' ? !!caps : false };
  const { chars, dead, sys } = m.keyChars(code, e, true, null);
  const flags = (alt ? IKF_ALT : 0) | (ctrl ? IKF_CTRL : 0) | (shift ? IKF_SHIFT : 0) | IKF_SYNTH | (dead ? IKF_DEAD : 0);
  const ch = [chars[0] ?? 0, chars[1] ?? 0, chars[2] ?? 0];
  return { vk: m.keyVk(code, e, m.CODES[code].vk, null), flags, nch: Math.min(chars.length, 3), ch, sys };
}

test('ns* keys: the C driver (input_web.c) writes what the page would', { skip: !CC && 'no wasi-sdk in deps/' }, async () => {
  const w = path.join(OUT, 'input_unit.wasm');
  execFileSync(CC, [...FLAGS, '-o', w, 'web/test/input_unit.c', 'runtime/web/thread_exit.c', ...LINK],
    { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  const out = execFileSync(process.execPath, [path.join(REPO, 'web/test/wasi-run.mjs'), w], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], timeout: 120_000 });
  const c = new Map();
  for (const l of out.split('\n')) {
    const f = l.split(' ');
    if (f[0] !== 'K') continue;
    const [dik, mods, caps, vk, flags, nch, c0, c1, c2, sys] = f.slice(1).map(Number);
    c.set(`${dik} ${mods} ${caps}`, { vk, flags, nch, ch: [c0, c1, c2], sys });
  }
  assert.match(out, /input_unit: \d+ key-down records/, 'input_unit ran to the end');
  const m = await keymap();
  const MODS = [0, ISYN_SHIFT, ISYN_CTRL, ISYN_ALT, ISYN_SHIFT | ISYN_CTRL, ISYN_SHIFT | ISYN_ALT, ISYN_CTRL | ISYN_ALT,
    ISYN_SHIFT | ISYN_CTRL | ISYN_ALT, ISYN_META, ISYN_META | ISYN_SHIFT];
  const isMod = code => /^(Shift|Control|Alt|Meta|OS)(Left|Right)$/.test(code) || code === 'CapsLock';
  const bad = [];
  let n = 0, keys = 0;
  for (let dik = 1; dik < 256; dik++) {
    const code = m.CODE_OF_DIK[dik];
    if (!code || isMod(code)) continue;
    keys++;
    for (const caps of [0, 1]) for (const mods of MODS) {
      const id = `${dik} ${mods} ${caps}`, want = pageRecord(m, dik, mods, caps), got = c.get(id) ?? null;
      n++;
      if (JSON.stringify(got) !== JSON.stringify(want))
        bad.push(`${code} mods ${mods} caps ${caps}: C ${JSON.stringify(got)}, page ${JSON.stringify(want)}`);
      c.delete(id);
    }
  }
  for (const id of c.keys()) bad.push(`C wrote a record the page has no key for: ${id}`);
  console.log(`${keys} keys, ${n} keystrokes compared, ${bad.length} differ${bad.length ? ':\n  ' + bad.slice(0, 30).join('\n  ') : ''}`);
  assert.ok(keys >= 100, 'every key compared');
  assert.deepEqual(bad, []);
  // the review's cases, spelled out
  const opt = (dik, mods) => pageRecord(m, dik, mods, 0);
  assert.deepEqual([opt(0x02, ISYN_ALT | ISYN_SHIFT).nch, opt(0x02, ISYN_ALT | ISYN_SHIFT).ch[0]], [1, 0x21], 'Option+Shift+1 types "!"');
  assert.deepEqual([opt(0x0C, ISYN_ALT | ISYN_SHIFT).nch, opt(0x0C, ISYN_ALT | ISYN_SHIFT).ch[0]], [1, 0x5F], 'Option+Shift+Minus types "_"');
  assert.deepEqual([opt(0x1E, ISYN_ALT).nch, opt(0x1E, ISYN_ALT).sys], [0, 0x61], 'Option+A is a command (WM_SYSCHAR a)');
  assert.deepEqual([opt(0x02, ISYN_ALT).nch, opt(0x02, ISYN_ALT).sys], [0, 0x31], 'Option+1 is a command');
});
