// runtime/web/inflate.h, which unpacks the texture packs' version 2 entries (texrep_web.c; tools/texpacks.py writes
// them with zlib's raw deflate): what Node's zlib packs it unpacks to the same bytes, and a short, long or broken
// stream it refuses without reading or writing outside its buffers.
//   node --test web/test/inflate.test.mjs      (needs clang)
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const OUT = path.join(REPO, 'out/web/test'); fs.mkdirSync(OUT, { recursive: true });
const haveClang = spawnSync('clang', ['--version'], { encoding: 'utf8' }).status === 0;

// stdin: records of (stream length, output size, stream); stdout per record: 1 or 0, and FNV-1a of what it unpacked
const HARNESS = `#include <stdio.h>
#include <stdlib.h>
#include "inflate.h"
int main(void){
    uint32_t h[2];
    while (fread(h, 4, 2, stdin) == 2){
        uint8_t *in = malloc(h[0] ? h[0] : 1), *out = malloc(h[1] ? h[1] : 1);
        if (!in || !out || fread(in, 1, h[0], stdin) != h[0]) return 2;
        int ok = inflate_raw(in, h[0], out, h[1]);
        uint32_t f = 2166136261u;
        if (ok) for (uint32_t i = 0; i < h[1]; i++) f = (f ^ out[i]) * 16777619u;
        printf("%d %08x\\n", ok, f);
        free(in); free(out);
    }
    return 0;
}
`;
const fnv = b => { let f = 2166136261; for (const x of b) f = Math.imul(f ^ x, 16777619) >>> 0; return f.toString(16).padStart(8, '0'); };

let seed = 11;
const rnd = n => { seed = (seed * 1103515245 + 12345) >>> 0; return (seed >>> 8) % n; };
/** the kinds of bytes a pack holds and some others: noise, text, zeros, runs, and DXT5-like 16-byte blocks that
 *  repeat (a map's flat areas), with an XOR delta's mostly-zero blocks */
function samples() {
  const s = [];
  s.push(['empty', Buffer.alloc(0)], ['one byte', Buffer.from([7])]);
  const noise = n => { const b = Buffer.alloc(n); for (let i = 0; i < n; i++) b[i] = rnd(256); return b; };
  s.push(['noise 70 KB', noise(70000)]);
  s.push(['zeros 300 KB', Buffer.alloc(300000)]);
  const words = ['map', 'road', 'Bayview', 'shortcut', 'the ', 'and ', '\n', '2048', 'DXT5'];
  let t = ''; while (t.length < 200000) t += words[rnd(words.length)] + (rnd(4) ? ' ' : '');
  s.push(['text 200 KB', Buffer.from(t)]);
  const runs = Buffer.alloc(150000); for (let i = 0, v = 0; i < runs.length; i++) { if (!rnd(200)) v = rnd(256); runs[i] = v; }
  s.push(['runs 150 KB', runs]);
  const blocks = [noise(16), noise(16), noise(16), noise(16)];
  const dxt = Buffer.alloc(16 * 40000);
  for (let i = 0; i < 40000; i++) (rnd(10) ? blocks[rnd(4)] : noise(16)).copy(dxt, i * 16);
  s.push(['DXT-like 640 KB', dxt]);
  const delta = Buffer.alloc(16 * 40000);
  for (let i = 0; i < 40000; i++) if (!rnd(9)) noise(16).copy(delta, i * 16);
  s.push(['XOR delta 640 KB', delta]);
  return s;
}

test('inflate.h unpacks what zlib packs, and refuses short, long and broken streams', { skip: !haveClang && 'no clang' }, () => {
  const dir = fs.mkdtempSync(path.join(OUT, 'inflate-c-'));
  try {
    const src = path.join(dir, 'inflate_check.c'), bin = path.join(dir, 'inflate_check');
    fs.writeFileSync(src, HARNESS);
    const san = spawnSync('clang', ['-fsanitize=address,undefined', '-x', 'c', '-o', path.join(dir, 'x'), '-'],
      { input: 'int main(void){return 0;}' }).status === 0 ? ['-fsanitize=address,undefined', '-fno-sanitize-recover=all'] : [];
    execFileSync('clang', ['-O1', '-g', ...san, '-Wall', '-Wextra', '-Werror', '-I', path.join(REPO, 'runtime/web'), '-o', bin, src]);
    const cases = [];                       // [what, stream, output size, expected FNV or null for refused]
    for (const [what, data] of samples()) {
      for (const [how, opt] of [['stored', { level: 0 }], ['fixed codes', { level: 6, strategy: zlib.constants.Z_FIXED }],
        ['level 1', { level: 1 }], ['level 9', { level: 9, memLevel: 9 }], ['Huffman only', { level: 6, strategy: zlib.constants.Z_HUFFMAN_ONLY }],
        ['RLE', { level: 6, strategy: zlib.constants.Z_RLE }]]) {
        const z = zlib.deflateRawSync(data, opt);
        cases.push([`${what}, ${how}`, z, data.length, fnv(data)]);
        if (data.length > 1 && how === 'level 9') {
          cases.push([`${what}, ${how}, one byte short`, z.subarray(0, z.length - 1), data.length, null]);
          cases.push([`${what}, ${how}, a byte too few out`, z, data.length - 1, null]);
          cases.push([`${what}, ${how}, a byte too many out`, z, data.length + 1, null]);
          for (let k = 0; k < 20; k++) {        // a flipped bit: refused, or (rarely) other bytes, but never out of bounds
            const bad = Buffer.from(z); const at = rnd(bad.length); bad[at] ^= 1 << rnd(8);
            cases.push([`${what}, ${how}, bit flipped at ${at}`, bad, data.length, 'any']);
          }
        }
      }
    }
    const input = Buffer.concat(cases.flatMap(([, z, n]) => { const h = Buffer.alloc(8); h.writeUInt32LE(z.length, 0); h.writeUInt32LE(n, 4); return [h, z]; }));
    const lines = execFileSync(bin, { input, encoding: 'utf8', timeout: 120_000, maxBuffer: 1 << 24 }).trim().split('\n');
    assert.equal(lines.length, cases.length);
    const wrong = [];
    cases.forEach(([what, , , want], i) => {
      const [ok, f] = lines[i].split(' ');
      if (want === 'any') return;
      if (want === null ? ok !== '0' : ok !== '1' || f !== want) wrong.push(`${what}: ${lines[i]}`);
    });
    assert.deepEqual(wrong, []);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
