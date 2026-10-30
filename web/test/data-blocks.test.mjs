// The server's /data-blocks/ (runs of 1 MiB blocks of a game file, compressed) against a made-up game folder,
// so it runs anywhere:
//   node --test web/test/data-blocks.test.mjs
// The manifest names it; every encoding gives back the file's exact bytes; a run that does not compress goes
// as it is (judged per run); requests out of bounds, outside the game's files or past the end are refused; a client holding all
// its compressors gets the rest as they are (never refused, never queued), and gets them back when it lets go; past
// the 128 compressors of all clients, the next client gets its runs as they are until one is given back.  A
// shared cache (Cloudflare's) may keep a run only when its address names this version of the file and it is as
// small as it gets.  One block to a client that takes br goes as the server keeps it once it does (compressed at
// brotli's best: the manifest names these at /data-blocks/v2/), and until then a shared cache keeps it a while only.
// Port: NFSU2_TEST_BLOCKS_PORT (8532).
// NFSU2_TEST_GAME=mw: the same against Most Wanted's server (--game mw): its program speed.exe, its music inside SOUND,
// every route under /mw (game-fixture.mjs).
import test from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import http from 'node:http';
import path from 'node:path';
import zlib from 'node:zlib';
import { spawn } from 'node:child_process';
import { serverCommand } from '../tools/server-cmd.mjs';
import { G } from './game-fixture.mjs';

const PORT = +(process.env.NFSU2_TEST_BLOCKS_PORT || 8532);
const M = 1 << 20;
const TMP = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'nfsu2-blocks-test-'));
test.after(() => fs.rmSync(TMP, { recursive: true, force: true }));

// random bytes (AES-CTR over zeros), or bytes of 2 bits each that compress to about a quarter
const bytes = (size, seed, packs) => {
  const b = crypto.createCipheriv('aes-128-ctr', crypto.createHash('sha256').update(seed).digest().subarray(0, 16), Buffer.alloc(16)).update(Buffer.alloc(size));
  if (packs) for (let i = 0; i < size; i++) b[i] = (b[i] & 3) * 0x41;
  return b;
};
const DATA = path.join(TMP, 'game');
// the game's program, and its music (at the top in Underground 2's PFDATA, inside SOUND in Most Wanted's)
const EXE = G.exe, MUS = G.id === 'ug2' ? 'PFDATA/MIX.MUS' : 'SOUND/PFDATA/MIX.MUS', B = G.base;
const FILES = { [EXE]: bytes(3 * M + 17, 'exe', false), 'TRACKS/L4RA.BUN': bytes(8 * M + 5, 'track', true),
  'MOVIES/INTRO.VP6': bytes(2 * M, 'movie', false),
  // a first block that does not compress before two that do (as the game's music): judged per run
  [MUS]: Buffer.concat([bytes(M, 'mix0', false), bytes(2 * M, 'mix1', true)]) };
for (const [p, b] of Object.entries(FILES)) { fs.mkdirSync(path.dirname(path.join(DATA, p)), { recursive: true }); fs.writeFileSync(path.join(DATA, p), b); }
fs.writeFileSync(path.join(DATA, 'NOTES.TXT'), 'not the game');
fs.writeFileSync(path.join(TMP, 'game.wasm'), Buffer.alloc(8));

/** the server; `compress`: the threads that keep blocks at brotli's best (none: the answers compressed as they are sent) */
async function server({ compress = 0 } = {}) {
  const st = path.join(TMP, `state${compress}`);
  const srv = spawn(...serverCommand(['--data', DATA, ...(G.id === 'ug2' ? [] : ['--game', G.id]), '--http-port', String(PORT), '--state', st, '--wasm', path.join(TMP, 'game.wasm')]),
    { stdio: ['ignore', 'pipe', 'pipe'], env: { ...process.env, NFSU2_COMPRESS_THREADS: String(compress) } });
  await new Promise((res, rej) => {
    let out = '';
    const t = setTimeout(() => rej(new Error('server did not start: ' + out)), 15000);
    srv.stdout.on('data', d => { out += d; if (out.includes('  access    :')) { clearTimeout(t); res(); } });
    srv.on('exit', c => { clearTimeout(t); rej(new Error('server exited ' + c + ' ' + out)); });
  });
  return { stop: async () => { const gone = srv.exitCode !== null ? null : new Promise(r => srv.once('exit', r)); srv.kill(); await gone; } };
}
/** an answer, its body as it came over the wire, and that body decoded */
function get(p, { ae = 'gzip, deflate, br, zstd', method = 'GET' } = {}) {
  return new Promise((res, rej) => {
    const q = http.request({ host: '127.0.0.1', port: PORT, path: B + p, method, headers: { host: `localhost:${PORT}`, 'accept-encoding': ae } }, r => {
      const c = []; r.on('data', d => c.push(d));
      r.on('end', () => {
        const wire = Buffer.concat(c), enc = r.headers['content-encoding'];
        const body = enc === 'zstd' ? zlib.zstdDecompressSync(wire) : enc === 'br' ? zlib.brotliDecompressSync(wire) : enc === 'gzip' ? zlib.gunzipSync(wire) : wire;
        res({ status: r.statusCode, headers: r.headers, wire, body });
      });
    });
    q.on('error', rej); q.end();
  });
}
const slice = (file, b, n) => FILES[file].subarray(b * M, Math.min(FILES[file].length, (b + n) * M));
/** each file's mtime as the manifest names it (the m of a run's address) */
const mtimes = async () => Object.fromEntries(JSON.parse((await get('/data-manifest', { ae: 'identity' })).body).files.filter(f => !f.d).map(f => [f.p, f.m]));
const KEEP = 'public, max-age=31536000, immutable', WHILE = 'public, max-age=600';
/** what a shared cache is told: 'keep' (for good, both CDN headers), 'while' (10 minutes) or 'no' (no-store) */
const edge = r => r.headers['cdn-cache-control'] === KEEP && r.headers['cloudflare-cdn-cache-control'] === KEEP ? 'keep'
  : r.headers['cdn-cache-control'] === WHILE && r.headers['cloudflare-cdn-cache-control'] === WHILE ? 'while'
  : r.headers['cdn-cache-control'] === 'no-store' && r.headers['cloudflare-cdn-cache-control'] === 'no-store' ? 'no' : JSON.stringify(r.headers);

test('/data-blocks/: named by the manifest, exact in every encoding, as they are where they do not compress, bounds refused', { timeout: 60_000 }, async () => {
  const s = await server();
  try {
    const man = JSON.parse((await get('/data-manifest', { ae: 'identity' })).body);
    assert.deepEqual(man.blocks, { url: `${B}/data-blocks/v2/`, size: M, max: 8 });

    // a run that compresses: every encoding the client may take, the exact bytes, smaller on the wire
    const encs = [...(zlib.zstdDecompressSync ? ['zstd'] : []), 'br', 'gzip'];
    for (const [ae, want] of [['gzip, deflate, br, zstd', encs[0]], ['gzip, br', 'br'], ['gzip', 'gzip'], ['identity', undefined]]) {
      const r = await get('/data-blocks/TRACKS/L4RA.BUN?b=1&n=8', { ae });
      assert.equal(r.status, 200, ae);
      assert.equal(r.headers['content-encoding'], want, ae);
      assert.ok(r.body.equals(slice('TRACKS/L4RA.BUN', 1, 8)), `exact bytes with ${ae}`);
      assert.match(r.headers.vary ?? '', /accept-encoding/i);
      assert.match(r.headers['cache-control'], /no-store/);
      assert.equal(r.headers['x-content-type-options'], 'nosniff');
      if (want) { assert.ok(r.wire.length < r.body.length * 0.5, `${ae}: ${r.wire.length} on the wire for ${r.body.length}`); assert.equal(r.headers['content-length'], undefined); }
      else assert.equal(+r.headers['content-length'], r.body.length);
    }
    // the last, short block; one block; a file that does not compress (random bytes, the movies' video)
    assert.ok((await get('/data-blocks/TRACKS/L4RA.BUN?b=8&n=1')).body.equals(slice('TRACKS/L4RA.BUN', 8, 1)));
    for (const [f, b, n, enc] of [[EXE, 0, 4, undefined], ['MOVIES/INTRO.VP6', 1, 1, undefined], [MUS, 0, 3, undefined],
      [MUS, 1, 2, encs[0]]]) {
      const r = await get(`/data-blocks/${f}?b=${b}&n=${n}`);
      assert.equal(r.headers['content-encoding'], enc, `${f} from block ${b}`);
      assert.ok(r.body.equals(slice(f, b, n)), f);
    }
    // HEAD: the headers of the plain answer, no body
    const h = await get('/data-blocks/TRACKS/L4RA.BUN?b=0&n=2', { method: 'HEAD' });
    assert.equal(h.status, 200); assert.equal(+h.headers['content-length'], 2 * M); assert.equal(h.wire.length, 0);

    // refused: out of bounds, past the end, not a game file, not inside the folder
    const status = async p => (await get(p)).status;
    for (const q of ['', '?b=0', '?b=0&n=0', '?b=0&n=9', '?b=-1&n=1', '?b=1e3&n=1', '?b=12345678&n=1', '?b=0&n=01x'])
      assert.equal(await status(`/data-blocks/${EXE}${q}`), 400, q);
    assert.equal(await status(`/data-blocks/${EXE}?b=4&n=1`), 416);       // 3 MiB + 17: blocks 0..3
    assert.equal(await status('/data-blocks/NOTES.TXT?b=0&n=1'), 404);
    assert.equal(await status('/data-blocks/TRACKS?b=0&n=1'), 404);
    assert.equal(await status('/data-blocks/TRACKS/.hidden?b=0&n=1'), 404);
    // (TRACKS/../<the program> is the program: the URL is resolved before anything is looked up, as for /data/)
    assert.equal(await status('/data-blocks/..%2fstate/etag.key?b=0&n=1'), 400);
  } finally { await s.stop(); }
});

test('/data-blocks/: a shared cache may keep a run for good: this version of the file, as small as it gets', { timeout: 60_000 }, async () => {
  const s = await server();
  try {
    const m = await mtimes();
    const run = (f, b, n, mt = m[f], ae, sz = FILES[f].length) =>
      get(`/data-blocks/${f}?b=${b}&n=${n}${mt === null ? '' : `&m=${mt}`}${sz === null ? '' : `&s=${sz}`}`, ae ? { ae } : undefined);
    // compressed, or a run that does not compress: kept, and still exact
    for (const [f, b, n, ae] of [['TRACKS/L4RA.BUN', 1, 8], ['TRACKS/L4RA.BUN', 0, 1, 'gzip, br'], [EXE, 0, 4], [MUS, 0, 1],
      ['MOVIES/INTRO.VP6', 0, 2]]) {
      const r = await run(f, b, n, m[f], ae);
      assert.equal(r.status, 200, f);
      assert.equal(edge(r), 'keep', `${f} from block ${b}`);
      assert.match(r.headers['cache-control'], /private, no-store/, 'the browser keeps none (the page has its own cache)');
      assert.ok(r.body.equals(slice(f, b, n)), f);
    }
    // not kept: no version, another version (mtime or size), a client that takes no compression
    assert.equal(edge(await run('TRACKS/L4RA.BUN', 1, 1, null)), 'no');
    assert.equal(edge(await run('TRACKS/L4RA.BUN', 1, 1, m['TRACKS/L4RA.BUN'] - 1)), 'no');
    assert.equal(edge(await run('TRACKS/L4RA.BUN', 1, 1, m['TRACKS/L4RA.BUN'], undefined, FILES['TRACKS/L4RA.BUN'].length + 1)), 'no');
    assert.equal(edge(await run('TRACKS/L4RA.BUN', 1, 1, m['TRACKS/L4RA.BUN'], 'identity')), 'no');
    // a page of before names no size: kept on the mtime
    assert.equal(edge(await run('TRACKS/L4RA.BUN', 1, 1, m['TRACKS/L4RA.BUN'], undefined, null)), 'keep');
  } finally { await s.stop(); }
});

test("/data-blocks/v2/: one block at brotli's best once the server keeps it, for good; till then a shared cache keeps it a while", { timeout: 120_000 }, async () => {
  const s = await server({ compress: 2 });
  try {
    const m = await mtimes();
    const block = (f, b, ae = 'gzip, deflate, br, zstd') => get(`/data-blocks/v2/${f}?b=${b}&n=1&m=${m[f]}&s=${FILES[f].length}`, { ae });
    // compressed as it is sent and kept a while, or kept already (the server keeps every block of the manifest, those
    // the answers ask for first)
    const first = await block('TRACKS/L4RA.BUN', 3);
    assert.ok(first.body.equals(slice('TRACKS/L4RA.BUN', 3, 1)));
    if (first.headers['content-encoding'] !== 'br' || first.headers['content-length'] === undefined) assert.equal(edge(first), 'while');
    // every block that compresses (not L4RA's last: 5 bytes), kept: br, its length, kept for good, exact, smaller than
    // gzip's
    const wanted = [...Array(8).keys()].map(b => ['TRACKS/L4RA.BUN', b]).concat([[MUS, 1], [MUS, 2]]);
    for (const [f, b] of wanted) {
      let r;
      for (let i = 0; i < 400; i++) {
        r = await block(f, b);
        if (r.headers['content-encoding'] === 'br' && r.headers['content-length'] !== undefined) break;
        await new Promise(res => setTimeout(res, 200));
      }
      assert.equal(r.headers['content-encoding'], 'br', `${f} block ${b} kept`);
      assert.equal(+r.headers['content-length'], r.wire.length);
      assert.equal(edge(r), 'keep', `${f} block ${b}`);
      assert.ok(r.body.equals(slice(f, b, 1)), `${f} block ${b} exact`);
      const gz = await block(f, b, 'gzip');
      assert.ok(r.wire.length < gz.wire.length, `${f} block ${b}: ${r.wire.length} kept, ${gz.wire.length} gzip`);
      assert.equal(edge(gz), 'keep', 'a client without br: compressed as it is sent, as before');
    }
    // noise as it is, for good once the server has found it does not compress; the old addresses the same
    for (const [f, b] of [['MOVIES/INTRO.VP6', 1], [MUS, 0], [EXE, 2]]) {
      let r;
      for (let i = 0; i < 400; i++) {
        r = await block(f, b);
        if (edge(r) === 'keep') break;
        assert.equal(edge(r), 'while', f);
        await new Promise(res => setTimeout(res, 200));
      }
      assert.equal(r.headers['content-encoding'], undefined, f);
      assert.equal(edge(r), 'keep', f);
      assert.ok(r.body.equals(slice(f, b, 1)), f);
    }
    const old = await get(`/data-blocks/TRACKS/L4RA.BUN?b=4&n=1&m=${m['TRACKS/L4RA.BUN']}&s=${FILES['TRACKS/L4RA.BUN'].length}`);
    assert.ok(old.wire.equals((await block('TRACKS/L4RA.BUN', 4)).wire));
  } finally { await s.stop(); }
});

test('/data-blocks/: a client holding all its compressors gets the rest as they are, and gets them back when it lets go', { timeout: 60_000 }, async () => {
  const s = await server();
  const held = [];
  try {
    // 8 compressed runs that are not read: each holds its compressor (about 2 MB compressed, more than the
    // socket takes), the 9th and 10th come as they are
    const open = () => new Promise((res, rej) => {
      const q = http.request({ host: '127.0.0.1', port: PORT, path: `${B}/data-blocks/TRACKS/L4RA.BUN?b=0&n=8`, headers: { host: `localhost:${PORT}`, 'accept-encoding': 'zstd, br, gzip' } }, r => {
        r.pause(); held.push(r); res(r.headers['content-encoding'] ?? 'none');
      });
      q.on('error', rej); q.end();
    });
    const encs = [];
    for (let i = 0; i < 10; i++) encs.push(await open());
    assert.deepEqual(encs.slice(0, 8).map(e => e !== 'none'), Array(8).fill(true), encs.join(' '));
    assert.deepEqual(encs.slice(8), ['none', 'none'], encs.join(' '));
    // the uncompressed answers are still exact, and no shared cache keeps one (compressed, it is a quarter)
    const plain = await get(`/data-blocks/TRACKS/L4RA.BUN?b=2&n=1&m=${(await mtimes())['TRACKS/L4RA.BUN']}&s=${FILES['TRACKS/L4RA.BUN'].length}`);
    assert.equal(plain.headers['content-encoding'], undefined);
    assert.ok(plain.body.equals(slice('TRACKS/L4RA.BUN', 2, 1)));
    assert.equal(edge(plain), 'no');
    // letting go (the client goes away mid-answer) gives the compressors back
    for (const r of held.splice(0)) r.destroy();
    let enc;
    for (let i = 0; i < 40 && !enc; i++) { await new Promise(r => setTimeout(r, 50)); enc = (await get('/data-blocks/TRACKS/L4RA.BUN?b=0&n=1')).headers['content-encoding']; }
    assert.ok(enc, 'compressed again once the held answers are gone');
  } finally { for (const r of held) r.destroy(); await s.stop(); }
});

test('/data-blocks/: past the 128 compressors of all clients, the next client gets its runs as they are until one is given back', { timeout: 60_000 }, async () => {
  const s = await server();
  const held = [];
  try {
    // a client is an X-Forwarded-For address (this machine is a trusted proxy); its compressed runs are not read
    const open = c => new Promise((res, rej) => {
      const q = http.request({ host: '127.0.0.1', port: PORT, path: `${B}/data-blocks/TRACKS/L4RA.BUN?b=0&n=8`,
        headers: { host: `localhost:${PORT}`, 'accept-encoding': 'zstd, br, gzip', 'x-forwarded-for': `192.0.2.${c}` } }, r => {
        r.pause(); held.push(r); res(r.headers['content-encoding'] ?? 'none');
      });
      q.on('error', rej); q.end();
    });
    // 16 clients take 8 each
    const encs = [];
    for (let c = 1; c <= 16; c++) for (let i = 0; i < 8; i++) encs.push(await open(c));
    assert.equal(encs.filter(e => e === 'none').length, 0, encs.join(' '));
    // a 17th, with all of its own share free, gets its run as it is
    assert.equal(await open(17), 'none');
    // one given back (its client goes away mid-answer): the 17th gets it
    held.shift().destroy();
    let enc = 'none';
    for (let i = 0; i < 40 && enc === 'none'; i++) { await new Promise(r => setTimeout(r, 50)); enc = await open(17); }
    assert.notEqual(enc, 'none', 'compressed again once one of the 128 is given back');
  } finally { for (const r of held) r.destroy(); await s.stop(); }
});
