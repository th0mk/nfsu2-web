// The start page's horns (web/src/app/horn-preview.ts, Settings' Listen button) against the game's (runtime/horn.c):
//   node --test web/test/horn-preview.test.mjs        (the comparison needs wasi-sdk in deps/)
// 1. every kind renders: sound, no overload, and as long as it should be;
// 2. every kind as horn.c renders it (tests/horn_render.c, built for wasm as the game is) and as the port does, held
//    the same time: the same length, and the same sound sample for sample (the port does horn.c's float arithmetic:
//    identical but where expf, sinf, cosf or exp2f round a last bit differently), so a horn changed in one and not
//    the other fails here.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const js = (await esbuild.build({ entryPoints: [path.join(REPO, 'web/src/app/horn-preview.ts')], bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const H = await import('data:text/javascript,' + encodeURIComponent(js));
const WASI = fs.readdirSync(path.join(REPO, 'deps')).filter(d => d.startsWith('wasi-sdk-')).map(d => path.join(REPO, 'deps', d)).pop();
const CC = WASI && path.join(WASI, 'bin/clang');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
// the game build's target and code generation (web/build.sh), as web/test/roam-wasm.test.mjs builds
const FLAGS = ['--target=wasm32-wasip1-threads', '-pthread', '-O3', '-msimd128', '-mbulk-memory', '-mnontrapping-fptoint',
  '-mtail-call', '-ffp-contract=off', '-Iruntime', '-Iruntime/web'];
const LINK = ['-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=268435456'];
const dB = x => 20 * Math.log10(Math.max(x, 1e-9));
const peak = a => a.reduce((m, x) => Math.max(m, Math.abs(x)), 0);

test('every horn renders', () => {
  assert.deepEqual(H.HORN_NAMES, ['dual', 'single', 'truck', 'train', 'klaxon', 'cucaracha', 'clown', 'trombone'], "horn.c's kinds, in its order");
  for (const name of H.HORN_NAMES) {
    const pcm = H.renderHorn(name), held = H.previewHold(name), p = dB(peak(pcm));
    assert.ok(pcm.every(Number.isFinite), `${name}: finite`);
    assert.ok(p > -16 && p < -6, `${name}: peak ${p.toFixed(1)} dBFS`);
    assert.ok(pcm.length / 48000 > held && pcm.length / 48000 < held + 1.6, `${name}: ${(pcm.length / 48000).toFixed(2)} s for ${held.toFixed(2)} s held`);
  }
  assert.deepEqual(H.renderHorn('kazoo', 0.5), H.renderHorn('dual', 0.5), 'a kind the game does not have: the dual horn');
});

test("the page's horns are horn.c's", { skip: !CC && 'no wasi-sdk in deps/' }, () => {
  const w = path.join(OUT, 'horn_render.wasm');
  execFileSync(CC, [...FLAGS, '-o', w, 'tests/horn_render.c', ...LINK], { cwd: REPO, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
  const dir = fs.mkdtempSync(path.join(OUT, 'horn-'));
  try {
    for (const name of H.HORN_NAMES) for (const [held, rate] of [[H.previewHold(name), 48000], [0.275, 44100]]) {
      execFileSync(process.execPath, [path.join(REPO, 'web/test/wasi-run.mjs'), w, name, String(held), String(rate), `/tmp/${name}.f32`],
        { encoding: 'utf8', env: { ...process.env, WASI_DIR: `/tmp=${dir}` }, stdio: ['ignore', 'pipe', 'pipe'], timeout: 120_000 });
      const b = fs.readFileSync(path.join(dir, `${name}.f32`));
      const c = new Float32Array(b.buffer, b.byteOffset, b.length / 4), t = H.renderHorn(name, held, rate);
      let diff = 0;
      for (let i = 0; i < Math.min(c.length, t.length); i++) diff = Math.max(diff, Math.abs(c[i] - t[i]));
      const at = `${name} held ${held.toFixed(2)} s at ${rate} Hz`;
      console.log(`${at}: ${(c.length / rate).toFixed(2)} s, peak ${dB(peak(c)).toFixed(1)} dBFS, largest difference ${dB(diff).toFixed(1)} dBFS`);
      assert.equal(t.length, c.length, `${at}: as long`);
      assert.ok(dB(diff) < -100, `${at}: the same sound (largest difference ${dB(diff).toFixed(1)} dBFS)`);
    }
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});
