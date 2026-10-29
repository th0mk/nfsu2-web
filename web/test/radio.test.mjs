// The radio (web/src/radio.ts), its link to the runtime (web/src/app/radio-link.ts) and its folder (web/src/radio-folder.ts) on fakes,
// and the runtime's side (runtime/radio.c) in tests/radio_unit.c.
//   node --test web/test/radio.test.mjs
import { test, mock } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const load = async f => await import('data:text/javascript;base64,' + Buffer.from(
  (await esbuild.build({ entryPoints: [path.join(ROOT, f)], bundle: true, format: 'esm', write: false })).outputFiles[0].text).toString('base64'));
const { Radio } = await load('web/src/radio.ts');
const { plaqueTags, gameText, tagsOf } = await load('web/src/radio-tags.ts');
const { pickMusic, keptFolder } = await load('web/src/radio-folder.ts');
const { RadioLink, RADIO_WORDS, RADIO_MAGIC, RADIO_VERSION } = await load('web/src/app/radio-link.ts');
const W = Object.fromEntries(RADIO_WORDS.map((n, i) => [n, i]));

const CTL = 16 + 32 * 24, RING = 1 << 12, TAG = 44, TEXT = CTL + RADIO_WORDS.length * 4;
const settle = async () => { for (let k = 0; k < 8; k++) await new Promise(r => setImmediate(r)); };
const pcm = frames => ({ numberOfChannels: 2, getChannelData: c => new Float32Array(frames).fill(c ? 0.5 : 0.25) });
const text = (m, p, k) => Buffer.from(m.buffer, TEXT, 2 * 3 * TAG).subarray((p * 3 + k) * TAG, (p * 3 + k + 1) * TAG).toString('latin1').replace(/\0.*/, '');
const lines = (m, p) => [0, 1, 2].map(k => text(m, p, k));
const handle = name => ({ kind: 'file', name, getFile: async () => ({ size: 1, arrayBuffer: async () => new ArrayBuffer(1) }) });
const bad = () => { throw new Error('bad file'); };
const storage = (kept = {}) => {
  globalThis.localStorage = { getItem: k => kept[k] ?? null, setItem: (k, v) => { kept[k] = String(v); } };
  return kept;
};

function setup(names, decode = async () => pcm(RING * 2)) {
  mock.timers.reset();
  mock.timers.enable({ apis: ['setInterval'] });
  globalThis.OfflineAudioContext = function () { this.decodeAudioData = decode; };
  const memory = new WebAssembly.Memory({ initial: 4, maximum: 4, shared: true });
  const w = new Int32Array(memory.buffer, CTL, RADIO_WORDS.length), log = [];
  w[W.ring] = 0x8000; w[W.ring_frames] = RING;
  const radio = new Radio(() => new RadioLink(memory, 0), l => log.push(l));
  radio.setFolder({ files: names.map(handle), name: 'Music' });
  const game = async (place, seq) => { w[W.req_index] = place; w[W.req_seq] = seq; mock.timers.tick(50); await settle(); };
  const started = async () => { await radio.start(); await settle(); };
  return { memory, w, radio, log, game, started };
}

test('the shared words are in the order of radio_proto.h', () => {
  const h = fs.readFileSync(path.join(ROOT, 'runtime/web/radio_proto.h'), 'utf8');
  const body = h.slice(h.indexOf('uint32_t radio_magic'), h.indexOf('} HostStatusRadio;'));
  assert.deepEqual([...body.matchAll(/uint32_t (\w+);/g)].map(m => m[1]), RADIO_WORDS);
});

test('a game whose block is short or not the radio\'s gets no link', () => {
  const memory = new WebAssembly.Memory({ initial: 4, maximum: 4, shared: true });
  const words = (magic, version) => { const w = new Int32Array(memory.buffer, CTL, 2); w[0] = magic; w[1] = version; };
  assert.equal(RadioLink.open(memory, 0), null);
  words(RADIO_MAGIC, RADIO_VERSION);
  assert.ok(RadioLink.open(memory, 0));
  words(RADIO_MAGIC, RADIO_VERSION + 1);
  assert.equal(RadioLink.open(memory, 0), null);
  for (const ptr of [memory.buffer.byteLength - 8, -4, 2, 1e12, NaN]) assert.equal(RadioLink.open(memory, ptr), null);
});

test('the rings take a track across their end and across the 32-bit counters; nothing is published without rings', () => {
  const memory = new WebAssembly.Memory({ initial: 4, maximum: 4, shared: true });
  const w = new Int32Array(memory.buffer, CTL, RADIO_WORDS.length), link = new RadioLink(memory, 0);
  assert.equal(link.serve(1, 100), false);
  assert.equal(link.push(new Float32Array(4), new Float32Array(4)), false);
  w[W.ring] = 0x8000; w[W.ring_frames] = RING;
  w[W.written] = w[W.read] = 0x7ffffffb;
  const left = Float32Array.from({ length: 10 }, (_, k) => k + 1);
  assert.equal(link.push(left, left.map(v => -v)), true);
  const ring = ch => new Float32Array(memory.buffer, 0x8000 + 4 * RING * ch, RING);
  assert.deepEqual([...ring(0).subarray(RING - 5), ...ring(0).subarray(0, 5)], [...left]);
  assert.deepEqual([...ring(1).subarray(0, 5)], [-6, -7, -8, -9, -10]);
  assert.deepEqual([w[W.written], link.room()], [0x7ffffffb + 10 | 0, RING - 10]);
});

test('each place is offered the first track with its three lines, the switch is set after the tags, and off clears it', async () => {
  const s = setup(['A - One.mp3', 'B - Two.mp3']);
  await s.started();
  assert.deepEqual([s.w[W.on], s.w[W.places], lines(s.memory, 0), lines(s.memory, 1)], [1, 3, ['A', 'One', 'Music'], ['A', 'One', 'Music']]);
  s.radio.stop(); assert.equal(s.w[W.on] & 1, 0);
});

test('a song is served from the queue of its place, the next track is offered, and a track is let go of when its last frame is in', async () => {
  const s = setup(['a.mp3', 'b.mp3', 'c.mp3']);
  await s.started();
  await s.game(0, 1);
  assert.deepEqual([s.w[W.ready_seq], s.w[W.written], text(s.memory, 0, 1), text(s.memory, 1, 1)], [1, RING, 'b', 'a']);
  s.w[W.read] = RING; await s.game(0, 1);
  assert.deepEqual([s.radio.song, s.w[W.written]], [null, RING * 2]);
});

test('songs that change quickly decode one track at a time: the one under way, then only the latest', async () => {
  let running = 0, most = 0, decodes = 0;
  const done = [];
  const s = setup(['a.mp3', 'b.mp3', 'c.mp3'], () => new Promise(r => { decodes++; most = Math.max(most, ++running); done.push(() => { running--; r(pcm(RING * 2)); }); }));
  await s.started();
  await s.game(0, 1);
  await s.game(0, 2);
  await s.game(1, 3);
  assert.equal(done.length, 1);
  done.shift()(); await settle();
  assert.deepEqual([done.length, s.w[W.ready_seq]], [1, 0]);
  done.shift()(); await settle();
  assert.deepEqual([most, decodes, s.w[W.ready_seq], s.radio.song?.at], [1, 2, 3, RING]);
});

test('a broken track gives way to the next; when every track is broken the game is told once and the radio goes off', async () => {
  const s = setup(['a.mp3', 'b.mp3', 'c.mp3']);
  s.radio.files[0].getFile = bad;
  await s.started();
  await s.game(0, 1);
  assert.deepEqual([s.w[W.ready_seq], s.w[W.failed_seq], s.radio.on], [1, 0, true]);
  assert.ok(s.log.some(l => /skips #1: Error: bad file/.test(l)));
  s.radio.files.forEach(f => { f.getFile = bad; });
  await s.game(0, 2);
  assert.deepEqual([s.w[W.failed_seq], s.w[W.on] & 1, s.radio.on], [2, 0, false]);
});

test('a track too large for a song is skipped without being read', async () => {
  const s = setup(['a.mp3', 'b.mp3']);
  let read = false;
  s.radio.files[0].getFile = async () => ({ size: 400 << 20, arrayBuffer: async () => { read = true; } });
  await s.started();
  await s.game(0, 1);
  assert.equal(read, false);
  assert.deepEqual([s.w[W.ready_seq], s.log.some(l => /skips #1: Error: 400 MB/.test(l))], [1, true]);
});

test('a track the rings stop taking gives way to the next; a lap of such tracks turns the radio off', async () => {
  const s = setup(['a.mp3', 'b.mp3']);
  await s.started();
  await s.game(0, 1);
  s.w[W.ring] = 0x7fff0000; s.w[W.read] = RING; await s.game(0, 1);
  assert.ok(s.log.some(l => /skips #1: Error: the rings are not where/.test(l)));
  s.w[W.ring] = 0x8000; await s.game(0, 1);
  assert.ok(s.log.some(l => /plays #2/.test(l)));
  s.w[W.ring] = 0x7fff0000; s.w[W.read] = s.w[W.written]; await s.game(0, 1);
  s.w[W.ring] = 0x8000; await s.game(0, 1);
  assert.ok(s.log.some(l => /off: no file of the folder plays/.test(l)));
  assert.equal(s.radio.on, false);
});

test('the radio goes off on the page when the runtime switched it off, and a start after off is served again', async () => {
  const s = setup(['a.mp3']);
  await s.started();
  s.w[W.on]++;
  mock.timers.tick(50); await settle();
  assert.deepEqual([s.radio.on, s.log.some(l => /off: the game switched it off/.test(l))], [false, true]);
  await s.started();
  await s.game(0, 1);
  assert.equal(s.w[W.ready_seq], 1);
});

test('the plaque keeps "&", has no artist in the title, lines that fit, ASCII letters, no empty line', () => {
  assert.deepEqual(plaqueTags({ artist: 'Rook & Vale', title: 'Rook & Vale - Long Way Back Home (Quiet Hours Remix) [LP]', album: 'x' }),
    { artist: 'Rook & Vale', title: 'Long Way Back Home (Quiet Hours Remix...', album: 'x' });
  assert.equal(gameText('Щука Zoë Łódź ★'), 'Shchuka Zoe Lodz ?');
  assert.deepEqual(plaqueTags({ artist: '', title: 'Привет', album: '' }, 'Моя'), { artist: 'Unknown artist', title: 'Privet', album: 'Moya' });
});

test('ID3 text frames: UTF-16 by its BOM (big-endian when none), a doubled BOM and a UTF-8 BOM', async () => {
  const be = t => Buffer.concat([...t].map(c => Buffer.from([c.charCodeAt(0) >> 8, c.charCodeAt(0) & 255]))), le = t => Buffer.from(t, 'utf16le');
  const frame = (id, ...body) => { const b = Buffer.concat(body.map(x => Buffer.from(x))), h = Buffer.alloc(10); h.write(id); h.writeUInt32BE(b.length, 4); return Buffer.concat([h, b]); };
  const file = (...frames) => { const f = Buffer.concat(frames), h = Buffer.alloc(10); h.write('ID3'); h[3] = 3; h[6] = (f.length >> 21) & 127; h[7] = (f.length >> 14) & 127; h[8] = (f.length >> 7) & 127; h[9] = f.length & 127; return Buffer.concat([h, f]); };
  const read = async buf => tagsOf({ name: 'x.mp3', getFile: async () => ({ slice: () => ({ arrayBuffer: async () => buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length) }) }) });
  assert.deepEqual(await read(file(
    frame('TIT2', [1, 0xfe, 0xff, 0xfe, 0xff], be('ヤラララ')), frame('TPE1', [1, 0xff, 0xfe], le('Zoë')), frame('TALB', [1], be('Alb')))),
    { title: 'ヤラララ', artist: 'Zoë', album: 'Alb' });
  assert.equal((await read(file(frame('TIT2', [3, 0xef, 0xbb, 0xbf], 'Ünï', [0], 'junk')))).title, 'Ünï');
});

test('a folder of 0, 1, 27, 28 or 10 000 files plays, and choosing it reads at most the tags of the tracks to come', async () => {
  for (const count of [0, 1, 27, 28, 10000]) {
    const s = setup(Array.from({ length: count }, (_, k) => `Song ${k}.mp3`));
    let reads = 0;
    s.radio.files.forEach(f => { f.getFile = async () => { reads++; return { size: 1, arrayBuffer: async () => new ArrayBuffer(1) }; }; });
    await s.started();
    assert.equal(s.radio.on, count > 0, `${count} files`);
    for (let seq = 1; count && seq <= 3; seq++) await s.game(seq % 2, seq);
    if (count) assert.deepEqual([s.w[W.ready_seq], s.w[W.failed_seq]], [3, 0], `${count} files`);
    assert.ok(reads <= 6, `${count} files: ${reads} reads`);
    s.radio.stop();
  }
});

const dir = (name, ...items) => ({ kind: 'directory', name, values: async function* () { yield* items; }, isSameEntry: async () => false });

test('subfolders are walked by name: nesting, empty ones, the same name in two, a loop, one that cannot be listed; no file is opened', async () => {
  const opened = [], file = name => ({ ...handle(name), getFile: async () => { opened.push(name); } });
  const back = dir('Back', file('never.mp3'));
  const root = dir('Music', file('b.mp3'), dir('Rock', file('x.mp3'), dir('Deep', file('z.mp3'))), dir('Empty'), dir('Pop', file('x.mp3')),
    { kind: 'directory', name: 'Locked', values() { throw new DOMException('no', 'NotAllowedError'); } }, back);
  root.isSameEntry = async o => o === back;
  globalThis.showDirectoryPicker = async () => root;
  const got = await pickMusic();
  delete globalThis.showDirectoryPicker;
  assert.deepEqual(got.paths, ['b.mp3', 'Rock/x.mp3', 'Rock/Deep/z.mp3', 'Pop/x.mp3']);
  const s = setup([]);
  s.radio.setFolder(got);
  assert.deepEqual([s.radio.trackPaths, opened], [['b.mp3', 'Pop/x.mp3', 'Rock/x.mp3', 'Rock/Deep/z.mp3'], []]);
});

test('without the directory picker a file input picks the folder; the files are not opened, a cancel picks nothing', async () => {
  delete globalThis.showDirectoryPicker;
  const file = path => ({ name: path.split('/').pop(), webkitRelativePath: path, size: 1 });
  const list = [file('Music/b.mp3'), file('Music/Rock/x.mp3'), file('Music/cover.jpg')];
  let chosen = list;
  globalThis.document = { createElement: () => {
    const l = {}, input = { addEventListener: (n, f) => { l[n] = f; }, click: () => queueMicrotask(() => { if (chosen) { input.files = chosen; l.change(); } else l.cancel(); }) };
    return input;
  } };
  const got = await pickMusic();
  assert.deepEqual([got.name, got.paths, await got.files[1].getFile()], ['Music', ['b.mp3', 'Rock/x.mp3'], list[1]]);
  chosen = null;
  assert.equal(await pickMusic(), null);
  delete globalThis.document;
  const s = setup([]);
  s.radio.setFolder(got);
  await s.started();
  assert.deepEqual([s.radio.on, s.radio.folder], [true, 'Music']);
});

test('each place is offered the first track whose mode suits it; Off is never chosen; a place with none is answered with nothing', async () => {
  storage();
  const s = setup(['a.mp3', 'b.mp3', 'c.mp3']);
  for (const [n, v] of Object.entries({ 'a.mp3': 'racing', 'b.mp3': 'menu', 'c.mp3': 'off' })) s.radio.setMode(n, v);
  await s.started();
  assert.deepEqual([text(s.memory, 0, 1), text(s.memory, 1, 1)], ['b', 'a']);
  s.radio.setMode('a.mp3', 'off'); await settle();
  assert.equal(s.w[W.places], 1);
  await s.game(1, 1);
  assert.deepEqual([s.w[W.failed_seq], s.w[W.on] & 1], [1, 1]);
});

test('modes are kept by path, a damaged value is not believed, a track not in the folder is forgotten, keys of earlier versions work', async () => {
  const kept = storage({ 'nfsu2.radio.modes': '{"b.mp3":"off","x.mp3":"menu","gone.mp3":"off"}' });
  const s = setup([]);
  s.radio.setFolder({ files: ['b.mp3', 'x.mp3', 'x.mp3'].map(handle), paths: ['b.mp3', 'Rock/x.mp3', 'Pop/x.mp3'], name: 'Music' });
  assert.deepEqual(['b.mp3', 'Pop/x.mp3', 'Rock/x.mp3'].map(p => s.radio.mode(p)), ['off', 'all', 'all']);
  s.radio.setMode('Rock/x.mp3', 'racing');
  s.radio.setMode('Pop/x.mp3', 'menu');
  s.radio.setMode('Pop/x.mp3', 'all');
  assert.deepEqual(JSON.parse(kept['nfsu2.radio.modes']), { 'b.mp3': 'off', 'Rock/x.mp3': 'racing' });
  for (const damaged of ['null', '[]', '7', '{"a.mp3":"loud"}', '{broken']) {
    kept['nfsu2.radio.modes'] = damaged;
    assert.equal(setup(['a.mp3']).radio.mode('a.mp3'), 'all', damaged);
  }
});

test('each place goes on from where it stopped: kept by path, a path not in the folder or a damaged value starts at the first track', async () => {
  const kept = storage();
  const s = setup(['a.mp3', 'b.mp3', 'c.mp3']);
  await s.started();
  await s.game(0, 1);
  await s.game(0, 2);
  assert.deepEqual([JSON.parse(kept['nfsu2.radio.next']), text(s.memory, 0, 1)], [['c.mp3', 'a.mp3'], 'c']);
  s.radio.stop();
  const again = setup(['a.mp3', 'b.mp3', 'c.mp3']);
  await again.started();
  assert.deepEqual([text(again.memory, 0, 1), text(again.memory, 1, 1)], ['c', 'a']);
  for (const damaged of ['{broken', 'null', '7', '["gone.mp3",5]']) {
    kept['nfsu2.radio.next'] = damaged;
    const first = setup(['a.mp3', 'b.mp3']);
    await first.started();
    assert.deepEqual([text(first.memory, 0, 1), text(first.memory, 1, 1)], ['a', 'a'], damaged);
    first.radio.stop();
  }
});

test('Shuffle plays any track that suits the place but the last one, is kept, and off goes back to the queue', async () => {
  const kept = storage();
  const random = Math.random;
  Math.random = () => 0.99;
  try {
    const s = setup(['a.mp3', 'b.mp3', 'c.mp3']);
    s.radio.setMode('b.mp3', 'racing');
    s.radio.setShuffle(true);
    assert.deepEqual([kept['nfsu2.radio.shuffle'], setup([]).radio.shuffle], ['1', true]);
    await s.started();
    assert.deepEqual([text(s.memory, 0, 1), text(s.memory, 1, 1)], ['c', 'c']);
    await s.game(0, 1);
    assert.deepEqual([text(s.memory, 0, 1), text(s.memory, 1, 1)], ['a', 'c']);
    s.radio.setShuffle(false); await settle();
    assert.deepEqual([kept['nfsu2.radio.shuffle'], text(s.memory, 0, 1), text(s.memory, 1, 1)], ['0', 'a', 'a']);
    s.radio.stop();
  } finally { Math.random = random; }
});

// The unit test is built for this machine: the clang of deps/wasi-sdk-* (as web/build.sh finds it) when it can build for the host (wasi-sdk's
// has the WebAssembly target only), else the system clang; with neither the test is skipped, as the wasi-sdk ones are.
const wasi = fs.existsSync(path.join(ROOT, 'deps')) ? fs.readdirSync(path.join(ROOT, 'deps')).filter(d => d.startsWith('wasi-sdk-')).map(d => path.join(ROOT, 'deps', d, 'bin/clang')) : [];
const hostClang = [...wasi.slice(-1), 'clang'].find(cc => /x86|aarch64|arm64/i.test(spawnSync(cc, ['--print-targets'], { encoding: 'utf8' }).stdout ?? ''));

test('the runtime: the song number, the tags and the scenarios of the game\'s songs (tests/radio_unit.c)', { skip: !hostClang && 'no clang that builds for this machine (deps/wasi-sdk-* builds WebAssembly only)' }, () => {
  const out = path.join(ROOT, 'out/web/test'), bin = path.join(out, 'radio_unit');
  fs.mkdirSync(out, { recursive: true });
  execFileSync(hostClang, ['-O1', '-Wall', '-Wextra', '-Werror', '-DNFSU2_RADIO_TEST', '-I', path.join(ROOT, 'runtime'), '-I', path.join(ROOT, 'tests'),
    '-o', bin, path.join(ROOT, 'tests/radio_unit.c')]);
  const got = execFileSync(bin, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim();
  assert.match(got, /radio_unit: 0 failed; scenarios: 0 of \d+ failed$/, got.split('\n').filter(l => !l.startsWith('ok ')).join('\n'));
});
