// The movies in AV1 (tools/movies_av1.py, web/src/host/movie.ts, runtime/movie_av1.c):
//   node --test web/test/movies-av1.test.mjs
// - which file the page opens for a movie: MOVIES/<name>.vp6 -> MOVIES/AV1/<name>.vp6 (a language pack's too), and
//   the movies a game counts (offline: "every movie kept"), with and without AV1, a pack without AV1 copies, the
//   preload leaving both kinds out
// - which movies a browser plays: the settings.ini line first, VP6 without WebCodecs
// - Firefox's BGRX pictures back to the YUV they were made from (BT.709, limited range), within a step of rounding
// - with NFSU2_TEST_DATA (the game folder) and an ffmpeg with libaom: the tool on THX_logo.vp6, its chunks as the
//   game's player reads them (the same ids in the same order, the sound's byte for byte, each video payload tagged,
//   a key frame at every MV0K)
// - Most Wanted (its page: the profile behind '@game' is src/games/mw, as build.mjs --game mw makes it): its movies'
//   AV1 copies, in any language the folder's English ones, the probe at its movie size (1024x512, Underground 2's
//   768x512), and with NFSMW_TEST_DATA (its game folder) the tool on ealogo_english_ntsc.vp6 (MVhd's 'VP60' kept)
// The modules are bundled from web/src with esbuild, as web/test/crews.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * from './web/src/host/datacache.ts'; export * as M from './web/src/host/movie.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const D = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
const { M } = D;
const jsMw = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral',
  alias: { '@game': path.join(ROOT, 'web/src/games/mw') } })).outputFiles[0].text;
const DM = await import('data:text/javascript;base64,' + Buffer.from(jsMw).toString('base64'));

test('a movie\'s AV1 copy', () => {
  assert.equal(D.av1Path('MOVIES/Scene01.vp6'), 'MOVIES/AV1/Scene01.vp6');
  assert.equal(D.av1Path('movies/psa.VP6'), 'movies/AV1/psa.VP6');
  assert.equal(D.av1Path('LANGPACKS/french/MOVIES/Scene01.vp6'), 'LANGPACKS/french/MOVIES/AV1/Scene01.vp6');
  for (const p of ['MOVIES/AV1/Scene01.vp6', 'MOVIES/movies.tag', 'TRACKS/STREAML4RA.BUN', 'LANGPACKS/french/MOVIES/AV1/PSA.vp6',
    'LANGPACKS/french/speech/x.vp6', 'Scene01.vp6'])
    assert.equal(D.av1Path(p), null, p);
});

test('the movies a game counts, in VP6 or AV1', () => {
  const f = (p) => ({ path: p, size: 10, mtime: 1 });
  const files = ['MOVIES/PSA.vp6', 'MOVIES/Scene01.vp6', 'MOVIES/ealogo.vp6', 'MOVIES/movies.tag', 'MOVIES/AV1/PSA.vp6',
    'MOVIES/AV1/Scene01.vp6', 'LANGPACKS/french/MOVIES/Scene01.vp6', 'LANGPACKS/french/MOVIES/AV1/Scene01.vp6',
    'LANGPACKS/danish/MOVIES/ealogo.vp6', 'TRACKS/L4RA.BUN'].map(f);
  const ps = (l) => l.map(x => x.path).sort();
  assert.deepEqual(ps(D.languageMovies(files, 'english')), ['MOVIES/PSA.vp6', 'MOVIES/Scene01.vp6', 'MOVIES/ealogo.vp6']);
  assert.deepEqual(ps(D.languageMovies(files, 'english', true)), ['MOVIES/AV1/PSA.vp6', 'MOVIES/AV1/Scene01.vp6', 'MOVIES/ealogo.vp6'],
    'the AV1 copies, the VP6 file of a movie without one');
  assert.deepEqual(ps(D.languageMovies(files, 'french', true)), ['LANGPACKS/french/MOVIES/AV1/Scene01.vp6', 'MOVIES/AV1/PSA.vp6', 'MOVIES/ealogo.vp6'],
    'the pack\'s own copy');
  assert.deepEqual(ps(D.languageMovies(files, 'danish', true)), ['LANGPACKS/danish/MOVIES/ealogo.vp6', 'MOVIES/AV1/PSA.vp6', 'MOVIES/AV1/Scene01.vp6'],
    'a pack movie without an AV1 copy: its VP6 one, not the folder\'s AV1');
  assert.deepEqual(ps(D.aheadFiles(files, 'french')), ['TRACKS/L4RA.BUN'], 'the preload leaves every movie out');
});

test('which movies a browser plays', async () => {
  assert.equal(await M.movieCodec({ NFSU2_MOVIE_CODEC: 'vp6' }), 'vp6');
  assert.equal(await M.movieCodec({ NFSU2_MOVIE_CODEC: ' AV1 ' }), 'av1');
  assert.equal(await M.movieCodec({ NFSU2_MOVIE_CODEC: '' }), typeof VideoDecoder === 'undefined' ? 'vp6' : 'av1', 'no WebCodecs: VP6');
});

test('RGB pictures back to their YUV', () => {
  // a picture in limited-range BT.709, each 2x2 of one chroma (as the decoder upsamples a flat block), to RGB as a
  // browser shows it, and back
  const w = 64, h = 32, ys = w + 96, uvs = ys / 2, Y0 = 0, U0 = ys * h, V0 = U0 + uvs * h / 2;
  const src = new Uint8Array(V0 + uvs * h / 2), rgb = new Uint8Array(w * h * 4), out = new Uint8Array(src.length);
  // colours inside the RGB cube (a clipped one cannot come back)
  for (let r = 0; r < h; r++) for (let c = 0; c < w; c++) src[Y0 + r * ys + c] = 40 + ((r * 7 + c * 3) % 160);
  for (let r = 0; r < h / 2; r++) for (let c = 0; c < w / 2; c++) { src[U0 + r * uvs + c] = 116 + (r * 5 + c) % 24; src[V0 + r * uvs + c] = 118 + (r + c * 3) % 20; }
  const kr = 0.2126, kb = 0.0722, kg = 1 - kr - kb;
  let clipped = 0;
  for (let r = 0; r < h; r++) for (let c = 0; c < w; c++) {
    const Y = (src[Y0 + r * ys + c] - 16) * 255 / 219, Cb = (src[U0 + (r >> 1) * uvs + (c >> 1)] - 128) * 255 / 224,
      Cr = (src[V0 + (r >> 1) * uvs + (c >> 1)] - 128) * 255 / 224;
    const R = Y + 2 * (1 - kr) * Cr, B = Y + 2 * (1 - kb) * Cb, G = (Y - kr * R - kb * B) / kg;
    const o = (r * w + c) * 4, cl = (x) => { const v = Math.round(x); if (v < 0 || v > 255) clipped++; return Math.max(0, Math.min(255, v)); };
    rgb[o + 2] = cl(R); rgb[o + 1] = cl(G); rgb[o] = cl(B); rgb[o + 3] = 255;      // BGRX
  }
  M.toI420(rgb, w, h, true, kr, kb, out, Y0, U0, V0, ys, uvs);
  let worstY = 0, worstC = 0;
  for (let r = 0; r < h; r++) for (let c = 0; c < w; c++) worstY = Math.max(worstY, Math.abs(out[Y0 + r * ys + c] - src[Y0 + r * ys + c]));
  for (let r = 0; r < h / 2; r++) for (let c = 0; c < w / 2; c++)
    worstC = Math.max(worstC, Math.abs(out[U0 + r * uvs + c] - src[U0 + r * uvs + c]), Math.abs(out[V0 + r * uvs + c] - src[V0 + r * uvs + c]));
  assert.equal(clipped, 0, 'the test picture fits RGB');
  assert.ok(worstY <= 1 && worstC <= 1, `Y off by ${worstY}, chroma by ${worstC}`);
  assert.equal(out[Y0 + ys - 1], 0, 'nothing written into the border');
});

test('Most Wanted: its movies\' AV1 copies, in any language the folder\'s, and the probe at its movie size', async () => {
  assert.equal(DM.av1Path('MOVIES/ealogo_english_ntsc.vp6'), 'MOVIES/AV1/ealogo_english_ntsc.vp6');
  assert.equal(DM.av1Path('MOVIES/AV1/ealogo_english_ntsc.vp6'), null);
  const f = (p) => ({ path: p, size: 10, mtime: 1 });
  const files = ['MOVIES/ealogo_english_ntsc.vp6', 'MOVIES/AV1/ealogo_english_ntsc.vp6', 'MOVIES/psa_english_ntsc.vp6',
    'MOVIES/AV1/psa_english_ntsc.vp6', 'MOVIES/blacklist_01_english_ntsc.vp6', 'GLOBAL/GLOBALA.BUN'].map(f);
  const ps = (l) => l.map(x => x.path).sort();
  for (const language of ['english', 'french', 'polish'])
    assert.deepEqual(ps(DM.languageMovies(files, language, true)), ['MOVIES/AV1/ealogo_english_ntsc.vp6', 'MOVIES/AV1/psa_english_ntsc.vp6',
      'MOVIES/blacklist_01_english_ntsc.vp6'], `${language}: the folder's movies (the runtime plays the English ones in any language)`);
  // the probe: each game asks the browser's decoder about its own movies' size
  const asked = [];
  globalThis.VideoDecoder = { isConfigSupported: async (c) => { asked.push(c); return { supported: c.codedWidth <= 1024 }; } };
  try {
    assert.equal(await DM.M.movieCodec({}), 'av1');
    assert.equal(await M.movieCodec({}), 'av1');
    assert.equal(await DM.M.movieCodec({}, { w: 1280, h: 720 }), 'vp6', 'a size the decoder refuses: VP6');
    assert.equal(await DM.M.movieCodec({ NFSU2_MOVIE_CODEC: 'vp6' }), 'vp6', 'the settings.ini line first');
  } finally { delete globalThis.VideoDecoder; }
  assert.deepEqual(asked.map(c => `${c.codec} ${c.codedWidth}x${c.codedHeight}`),
    ['av01.0.04M.08 1024x512', 'av01.0.04M.08 768x512', 'av01.0.04M.08 1280x720'], 'Most Wanted 1024x512, Underground 2 768x512');
});

const DATA = process.env.NFSU2_TEST_DATA, MWDATA = process.env.NFSMW_TEST_DATA;
const ffmpeg = spawnSync('ffmpeg', ['-hide_banner', '-encoders'], { encoding: 'utf8' });
const python = process.platform === 'win32' ? 'python' : 'python3';
const noTool = (data, what) => !data ? `${what} not set` : !/libaom-av1/.test(ffmpeg.stdout ?? '') ? 'no ffmpeg with libaom' : false;
/** the tool on one movie of a game folder: its chunks as the game's player reads them */
function toolCase(data, name) {
  const out = fs.mkdtempSync(path.join(os.tmpdir(), 'av1-'));
  try {
    const run = execFileSync(python, [path.join(ROOT, 'tools/movies_av1.py'), data, out, '--only', `MOVIES/${name}`, '--cpu-used', '8', '--jobs', '2'], { encoding: 'utf8' });
    assert.ok(run.includes(`"file": "MOVIES/${name}.vp6"`), run);
    const chunks = (b) => { const l = []; for (let o = 0; o + 8 <= b.length;) { const s = b.readUInt32LE(o + 4); l.push({ id: b.toString('latin1', o, o + 4), o, s }); o += s; } return l; };
    const vp6 = fs.readFileSync(path.join(data, `MOVIES/${name}.vp6`)), av1 = fs.readFileSync(path.join(out, `MOVIES/AV1/${name}.vp6`));
    const a = chunks(vp6), b = chunks(av1);
    assert.deepEqual(b.map(c => c.id), a.map(c => c.id), 'the same chunks in the same order');
    assert.equal(b.reduce((n, c) => n + c.s, 0), av1.length, 'the chunks fill the file');
    for (let i = 0; i < a.length; i++) {
      const x = a[i], y = b[i];
      if (x.id === 'MV0K' || x.id === 'MV0F') {
        assert.equal(av1.toString('latin1', y.o + 8, y.o + 12), 'AV01');
        if (x.id === 'MV0K') assert.equal(av1[y.o + 12] & 1, 1, `chunk ${i}: MV0K holds a key frame`);
      } else if (x.id !== 'MVhd') assert.ok(vp6.subarray(x.o, x.o + x.s).equals(av1.subarray(y.o, y.o + y.s)), `chunk ${i} (${x.id}) as it was`);
    }
    assert.ok(vp6.subarray(8, 0x14).equals(av1.subarray(8, 0x14)) && vp6.subarray(0x18, 0x20).equals(av1.subarray(0x18, 0x20)), 'the header as it was');
    assert.ok(av1.readUInt32LE(0x14) >= vp6.readUInt32LE(0x14), 'largest frame no smaller');
    assert.ok(av1.length < vp6.length, `${av1.length} bytes, VP6 ${vp6.length}`);
    return { vp6, av1 };
  } finally { fs.rmSync(out, { recursive: true, force: true }); }
}
test('the tool, on THX_logo.vp6', { skip: noTool(DATA, 'NFSU2_TEST_DATA (the game folder)') }, () => { toolCase(DATA, 'THX_logo'); });
test('the tool, on Most Wanted\'s ealogo_english_ntsc.vp6', { skip: noTool(MWDATA, 'NFSMW_TEST_DATA (Most Wanted\'s game folder)') }, () => {
  const { av1 } = toolCase(MWDATA, 'ealogo_english_ntsc');
  assert.equal(av1.toString('latin1', 8, 12), '06PV', 'MVhd +8: the codec FourCC VP60, kept');
  assert.deepEqual([av1.readUInt16LE(12), av1.readUInt16LE(14)], [1024, 512]);
});
