// Transmission Whine's sounds (runtime/whine.c, tools/whinepack.py, web/src/host/datacache.ts):
//   node --test web/test/whine.test.mjs
// - the preload fetches MODPACKS/WHINE only with the whine on, and the rest of MODPACKS either way
// - tools/whinepack.py writes MODPACKS/WHINE's three sounds from the mod's release (a zip, or its files extracted): the
//   1.0.2 release's own unless --any, 16-bit mono WAVs only, all three or nothing
// The modules are bundled from web/src with esbuild, as web/test/songs.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * from './web/src/host/datacache.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const D = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

test('the preload: MODPACKS/WHINE only with the whine on', () => {
  const files = ['MODPACKS/WHINE/car_whine.wav', 'MODPACKS/WHINE/car_whine_2.wav', 'MODPACKS/WHINE/car_whine_reverse.wav',
    'MODPACKS/PARTS.BIN', 'TRACKS/L4RA.BUN'].map(p => ({ path: p, size: 10, mtime: 1 }));
  const ahead = (packs) => D.aheadFiles(files, 'english', packs).map(x => x.path).sort();
  const without = ['MODPACKS/PARTS.BIN', 'TRACKS/L4RA.BUN'];
  assert.deepEqual(ahead(), without, 'no packs: no whine');
  assert.deepEqual(ahead({ texpack: '', songs: '' }), without, 'a page of before (no whine in its packs)');
  assert.deepEqual(ahead({ texpack: '', songs: '', whine: false }), without, 'off');
  assert.deepEqual(ahead({ texpack: '', songs: '', whine: true }), [...without, ...files.slice(0, 3).map(f => f.path)].sort(), 'on');
});

/** a WAV of n samples: channels, bits */
function wav(n, { channels = 1, bits = 16, rate = 44100 } = {}) {
  const data = Buffer.alloc(n * channels * bits / 8);
  for (let i = 0; i < n; i++) data.writeInt16LE(Math.round(8000 * Math.sin(i / 7)), i * channels * 2);
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8); h.write('fmt ', 12); h.writeUInt32LE(16, 16);
  h.writeUInt16LE(1, 20); h.writeUInt16LE(channels, 22); h.writeUInt32LE(rate, 24); h.writeUInt32LE(rate * channels * bits / 8, 28);
  h.writeUInt16LE(channels * bits / 8, 32); h.writeUInt16LE(bits, 34); h.write('data', 36); h.writeUInt32LE(data.length, 40);
  return Buffer.concat([h, data]);
}
const python = process.platform === 'win32' ? 'python' : 'python3';
const NAMES = ['car_whine.wav', 'car_whine_2.wav', 'car_whine_reverse.wav'];
const pack = (folder, mod, ...more) => spawnSync(python, ['-I', path.join(ROOT, 'tools/whinepack.py'), folder, '--mod', mod, ...more], { encoding: 'utf8' });
const havePython = spawnSync(python, ['--version']).status === 0;

test('tools/whinepack.py: the three sounds, checked', { skip: !havePython && 'no Python' }, () => {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'whinepack-'));
  try {
    const mod = path.join(tmp, 'mod'), snd = path.join(mod, 'UG2WhineGen', 'snd');
    fs.mkdirSync(snd, { recursive: true });
    for (const n of NAMES) fs.writeFileSync(path.join(snd, n), wav(1000));
    fs.writeFileSync(path.join(snd, 'snd_on.wav'), wav(10));
    const out = path.join(tmp, 'game');
    let r = pack(out, mod);
    assert.notEqual(r.status, 0, 'not the release\'s sounds: refused');
    assert.match(r.stderr, /not the 1\.0\.2 release's/);
    assert.ok(!fs.existsSync(path.join(out, 'MODPACKS')), 'nothing written');
    r = pack(out, mod, '--any');
    assert.equal(r.status, 0, r.stderr);
    assert.deepEqual(fs.readdirSync(path.join(out, 'MODPACKS', 'WHINE')).sort(), [...NAMES].sort(), 'the three sounds only (no chimes)');
    for (const n of NAMES) assert.deepEqual(fs.readFileSync(path.join(out, 'MODPACKS', 'WHINE', n)), wav(1000), `${n} as it is`);
    // from a zip, as the release comes
    const zip = path.join(tmp, 'mod.zip');
    const z = spawnSync(python, ['-I', '-c', 'import sys, zipfile, os\nwith zipfile.ZipFile(sys.argv[1], "w") as z:\n  for n in sys.argv[3:]: z.write(os.path.join(sys.argv[2], n), "snd/" + n)',
      zip, snd, ...NAMES], { encoding: 'utf8' });
    assert.equal(z.status, 0, z.stderr);
    const out2 = path.join(tmp, 'game2');
    r = pack(out2, zip, '--any');
    assert.equal(r.status, 0, r.stderr);
    assert.equal(fs.readdirSync(path.join(out2, 'MODPACKS', 'WHINE')).length, 3);
    // a stereo sound, and one missing
    fs.writeFileSync(path.join(snd, 'car_whine_2.wav'), wav(1000, { channels: 2 }));
    r = pack(path.join(tmp, 'game3'), mod, '--any');
    assert.notEqual(r.status, 0); assert.match(r.stderr, /car_whine_2\.wav: .*not 16-bit mono PCM/);
    fs.rmSync(path.join(snd, 'car_whine_reverse.wav'));
    r = pack(path.join(tmp, 'game4'), mod, '--any');
    assert.notEqual(r.status, 0); assert.match(r.stderr, /no snd\/car_whine_reverse\.wav/);
  } finally {
    fs.rmSync(tmp, { recursive: true, force: true });
  }
});
