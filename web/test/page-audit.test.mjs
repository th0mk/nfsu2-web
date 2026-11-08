// The page's fixes for the security and privacy audit that run under node:
//   node --test web/test/page-audit.test.mjs
// - PAGE-3: settings.ini's number check in linear time, the same answers as before; a line longer than
//           bundle.m's 1199 bytes is read in pieces, as fgets does
// - PAGE-2: settings.ini cannot set the page's own variables (files of the saves, automation), every line
//           outside the table is listed, a variable set by its own name is found for its row, and a restore
//           says what a backup's settings.ini changes on the network
// - PAGE-4: restoring a zip never inflates an entry past the size it declares (a zip bomb)
// - PAGE-1: the page address's environment cannot set the mounts or the page's own variables either
// - the audit review's PAGE-2 finding: a Settings row's change removes the line that would still win, found by
//           its line in the file (not by the 1199-byte pieces a long line before it is read in)
// The modules are bundled from web/src with esbuild, as web/test/restore-limit.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import zlib from 'node:zlib';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
// (namespaces, so a missing function fails its own test only: the file runs against the code before the audit too)
const entry = `export * from './web/src/app/settings.ts'; export * as SV from './web/src/app/saves-ui.ts';
export { zip, unzip, crc32, ZipError } from './web/src/app/zip.ts'; export * as UI from './web/src/app/settings-ui.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const S = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

// the number check before the audit (settings.ts at 29f3b86 and 14d6e46), to compare answers with
const OLD = /^[+-]?((\d+\.?\d*|\.\d+)([eE][+-]?\d+)?|0[xX]([0-9a-fA-F]+\.?[0-9a-fA-F]*|\.[0-9a-fA-F]+)([pP][+-]?\d+)?|inf(inity)?|nan(\([0-9A-Za-z_]*\))?)$/i;
const oldIsNumber = v => !!v && OLD.test(v.replace(/^[ \t\n\r\v\f]+/, ''));

test('PAGE-3: the number check gives the same answers, in linear time', () => {
  const vectors = ['1', '1.', '.5', '1.5e3', '0x1p3', '0x.8', 'inf', 'nan(abc)', '1.2.3', '', ' 7', '-0x1.8p-2', 'e5', '.', '0x', '1e', '+.5e-3',
    'INFINITY', 'nan()', '12abc', '0x1.', '0xA.Bp+4', '00', '+', '-.', '1e+', '9000000', '0.08', '1.0', 'Infinity', 'NaN', '0X1P-1', '5x', '1,5'];
  for (const v of vectors) assert.equal(S.isNumber(v), oldIsNumber(v), JSON.stringify(v));
  const times = {};
  // growing sizes, each checked at once (the quadratic check would take minutes for the larger ones)
  for (const n of [20_000, 150_000, 1_000_000]) {
    for (const [name, v] of [['digits', '1'.repeat(n) + 'x'], ['hex', '0x' + 'a'.repeat(n) + 'x']]) {
      const t0 = performance.now();
      assert.equal(S.isNumber(v), false);
      times[`${name} ${n}`] = +(performance.now() - t0).toFixed(1);
      assert.ok(times[`${name} ${n}`] < 200, `${name} ${n}: ${times[`${name} ${n}`]} ms, well under a second`);
    }
  }
  console.log('ms for a bad value of n digits:', JSON.stringify(times));
  // the old check on a value of 20,000 digits, for scale
  const t0 = performance.now(); oldIsNumber('1'.repeat(20_000) + 'x');
  console.log(`the old check, 20,000 digits: ${(performance.now() - t0).toFixed(0)} ms`);
});

test('PAGE-3: a long line is read in pieces of 1199 bytes, as bundle.m\'s fgets reads it; resolving stays fast', () => {
  const t1 = performance.now();
  S.resolveEnv(`max_pixels = ${'1'.repeat(20_000)}x\n`, {}, {});
  assert.ok(performance.now() - t1 < 100, `a 20 KB line resolved in ${(performance.now() - t1).toFixed(0)} ms`);
  const long = 'max_pixels = ' + '1'.repeat(1_000_000) + 'x';
  const t0 = performance.now();
  const r = S.resolveEnv(`movies = off\n${long}\nfps = 120\n`, {}, {});
  const ms = performance.now() - t0;
  console.log(`a 1 MB line: resolved in ${ms.toFixed(0)} ms; ${r.notes.length} notes; NFSMAXPIX = ${String(r.env.NFSMAXPIX).length} digits`);
  assert.ok(ms < 1000, `${ms} ms`);
  // fgets: the first piece is "max_pixels = " and 1186 digits (a number), the rest have no '='; the lines after keep their numbers
  assert.equal(r.env.NFSMAXPIX, '1'.repeat(1199 - 'max_pixels = '.length));
  assert.equal(r.env.NOMOVIES, '1'); assert.equal(r.env.NFSU2_FPS, '120');
  const pieces = Math.ceil((long.length + 1) / 1199);
  assert.equal(r.byEnv.NFSU2_FPS.line, 2 + pieces, 'line numbers as bundle.m counts them');
  // a short file reads as before
  assert.deepEqual(S.parseLines('a = 1\r\n# c\nb = 2').map(l => [l.line, l.key, l.value]), [[1, 'a', '1'], [3, 'b', '2']]);
});

test('PAGE-2: settings.ini cannot set the page\'s own variables; lines outside the table are listed', () => {
  const ini = ['# NFSU2 settings, read at every launch.', '#roam = off', 'NFSU2_ROAM = 1', 'NFSU2_ROAM_NAME = SneakyRoamer',
    'NFSU2_REGFILE = /save/NFS Underground 2/VICTIM/VICTIM', 'NFSU2_INPUT_SCRIPT = /save/evil.txt', 'NFSU2_LOG = /save/x.log',
    'NFSU2_CAREER_BOT = 1', 'some_path = ../x', 'NFSU2_STATUS_EVERY = 600', 'HOME = /elsewhere', ''].join('\n');
  const r = S.resolveEnv(ini, {}, { NFSU2_REGFILE: '/save/registry.txt', HOME: '/save' });
  assert.equal(r.env.NFSU2_REGFILE, '/save/registry.txt', 'the registry stays the page\'s');
  for (const k of ['NFSU2_INPUT_SCRIPT', 'NFSU2_LOG', 'NFSU2_CAREER_BOT', 'NFSU2_SOME_PATH']) assert.ok(!(k in r.env), `${k} not set`);
  assert.equal(r.env.HOME, '/save');
  assert.equal(r.env.NFSU2_STATUS_EVERY, '600', 'an ordinary runtime variable still works');
  // the free-roam switch's variable, set by its own name: found for the row (which shows "on" now)
  assert.equal(r.env.NFSU2_ROAM, '1');
  assert.deepEqual(r.byEnv.NFSU2_ROAM, { line: 3, phys: 3, key: 'NFSU2_ROAM' });
  // every line outside the table is listed, with what became of it
  const other = Object.fromEntries(r.other.map(o => [o.key, o.refused ?? `sets ${o.env}`]));
  console.log('lines outside the table:', JSON.stringify(other));
  assert.equal(other.NFSU2_REGFILE, 'only the page sets it');
  assert.equal(other.NFSU2_ROAM, 'sets NFSU2_ROAM');
  assert.equal(other.HOME, 'fixed in the browser');
  // the page address cannot set them either (shell.ts takes the address only on a --dev server, and even then not these)
  const a = S.resolveEnv(null, { NFSU2_REGFILE: '/save/NFS Underground 2/V/V', HOME: '/x', NFSU2_SAVEDIR: '/y', NOMOVIES: '1' }, { NFSU2_REGFILE: '/save/registry.txt', HOME: '/save', NFSU2_SAVEDIR: '/save' });
  assert.deepEqual([a.env.NFSU2_REGFILE, a.env.HOME, a.env.NFSU2_SAVEDIR, a.env.NOMOVIES], ['/save/registry.txt', '/save', '/save', '1']);
});

test('PAGE-2: a restore names what the backup\'s settings.ini does on the network', () => {
  const w = S.SV.settingsWarnings('#roam = off\nNFSU2_ROAM = 1\nNFSU2_ROAM_NAME = HiddenRoamer\nNFSU2_INPUT_SCRIPT = /save/s.txt\n');
  console.log('restore says:', JSON.stringify(w));
  assert.ok(w.some(x => /turns on free-roam multiplayer/.test(x) && x.includes('HiddenRoamer')));
  assert.ok(w.some(x => x.includes('NFSU2_ROAM=1')));
  assert.ok(w.some(x => /Ignored, only the page sets them: NFSU2_INPUT_SCRIPT/.test(x)));
  assert.deepEqual(S.SV.settingsWarnings('# NFSU2 settings\nmovies = off\nfps = 120\n'), [], 'an ordinary file: nothing to say');
});

// a zip with one deflated entry that declares `declared` bytes and inflates to `real` bytes of zeros
function bomb(real, declared = 100) {
  const data = zlib.deflateRawSync(Buffer.alloc(real), { level: 9 });
  const name = Buffer.from('registry.txt');
  const lh = Buffer.alloc(30); lh.writeUInt32LE(0x04034b50, 0); lh.writeUInt16LE(20, 4); lh.writeUInt16LE(8, 8);
  lh.writeUInt32LE(0, 14); lh.writeUInt32LE(data.length, 18); lh.writeUInt32LE(declared, 22); lh.writeUInt16LE(name.length, 26);
  const ch = Buffer.alloc(46); ch.writeUInt32LE(0x02014b50, 0); ch.writeUInt16LE(20, 6); ch.writeUInt16LE(8, 10);
  ch.writeUInt32LE(0, 16); ch.writeUInt32LE(data.length, 20); ch.writeUInt32LE(declared, 24); ch.writeUInt16LE(name.length, 28);
  const end = Buffer.alloc(22); end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(1, 8); end.writeUInt16LE(1, 10);
  end.writeUInt32LE(46 + name.length, 12); end.writeUInt32LE(30 + name.length + data.length, 16);
  return new Uint8Array(Buffer.concat([lh, name, data, ch, name, end]));
}
test('PAGE-4: an entry that inflates past its declared size is stopped there (a zip bomb)', async () => {
  const z = bomb(512 << 20);
  const m0 = process.memoryUsage().arrayBuffers;
  const t0 = performance.now();
  let err = null;
  try { await S.SV.readBackup(z); } catch (e) { err = e; }
  const ms = performance.now() - t0, grew = (process.memoryUsage().arrayBuffers - m0) / 1048576;
  console.log(`a ${(z.length / 1024).toFixed(0)} KB zip declaring 100 bytes, inflating to 512 MB: ${err?.message} after ${ms.toFixed(0)} ms, buffers +${grew.toFixed(0)} MB`);
  assert.ok(err instanceof S.ZipError, String(err));
  assert.match(err.message, /larger than it says/);
  assert.ok(grew < 64, `nothing near the inflated size is held (+${grew.toFixed(0)} MB)`);
  assert.ok(ms < 2000, `stopped at once (${ms.toFixed(0)} ms)`);
  // an honest deflated entry still restores
  const data = Buffer.from('NotFirstTime=1\n'.repeat(100));
  const comp = zlib.deflateRawSync(data);
  const name = Buffer.from('registry.txt');
  const crc = S.crc32(new Uint8Array(data));
  const lh = Buffer.alloc(30); lh.writeUInt32LE(0x04034b50, 0); lh.writeUInt16LE(20, 4); lh.writeUInt16LE(8, 8); lh.writeUInt32LE(crc, 14);
  lh.writeUInt32LE(comp.length, 18); lh.writeUInt32LE(data.length, 22); lh.writeUInt16LE(name.length, 26);
  const ch = Buffer.alloc(46); ch.writeUInt32LE(0x02014b50, 0); ch.writeUInt16LE(20, 6); ch.writeUInt16LE(8, 10); ch.writeUInt32LE(crc, 16);
  ch.writeUInt32LE(comp.length, 20); ch.writeUInt32LE(data.length, 24); ch.writeUInt16LE(name.length, 28);
  const end = Buffer.alloc(22); end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(1, 8); end.writeUInt16LE(1, 10);
  end.writeUInt32LE(46 + name.length, 12); end.writeUInt32LE(30 + name.length + comp.length, 16);
  const r = await S.SV.readBackup(new Uint8Array(Buffer.concat([lh, name, comp, ch, name, end])));
  assert.equal(Buffer.from(r.entries[0].data).toString(), data.toString());
  // a stored entry whose two sizes disagree is refused
  const st = S.zip([{ name: 'settings.ini', data: new TextEncoder().encode('movies = off\n') }]);
  const v = new DataView(st.buffer); v.setUint32(st.length - 22 - 46 - 'settings.ini'.length + 24, 99999, true);
  await assert.rejects(S.SV.readBackup(st), /damaged/);
});

test('PAGE-2 (review): a row change removes the line that would still win, by its line in the file', () => {
  // the reviewer's file: a 3000-character comment (read in three pieces), then a raw line that turns free roam on
  // before the row's own line, and the name
  const ini = ['# NFSU2 settings, read at every launch.', '# ' + 'x'.repeat(3000), 'NFSU2_ROAM = 1', 'roam = off', 'roam_name = Alice', 'fps = 120', ''].join('\n');
  const r0 = S.resolveEnv(ini, {}, {});
  assert.equal(r0.env.NFSU2_ROAM, '1', 'the raw line wins (the first that sets the variable)');
  assert.deepEqual(r0.byEnv.NFSU2_ROAM, { line: 5, phys: 3, key: 'NFSU2_ROAM' }, 'piece 5 as bundle.m counts, line 3 of the file');
  // turning the switch off (it showed on: the raw line applies)
  const { text, removed } = S.putKey(ini, 'roam', 'off');
  const r = S.resolveEnv(text, {}, {});
  console.log(`after "Free-roam multiplayer: off": removed ${removed}; NFSU2_ROAM=${r.env.NFSU2_ROAM} from ${JSON.stringify(r.byEnv.NFSU2_ROAM)}; lines ${JSON.stringify(text.split('\n').map(l => l.length > 40 ? l.slice(0, 12) + '…' : l))}`);
  assert.equal(removed, 1);
  assert.equal(r.env.NFSU2_ROAM, '0', 'free roam is off');
  assert.equal(r.byEnv.NFSU2_ROAM.key, 'roam', 'by the row\'s own line');
  assert.ok(!text.split('\n').includes('NFSU2_ROAM = 1'), 'the raw line is gone');
  assert.equal(r.env.NFSU2_ROAM_NAME, 'Alice', 'the name is kept');
  assert.equal(r.env.NFSU2_FPS, '120');
  assert.ok(text.includes('# ' + 'x'.repeat(3000)), 'the long comment is kept');
  // two raw lines before the row's own: both go; a raw line after it changes nothing (the row's line wins)
  const two = S.putKey('# ' + 'y'.repeat(2500) + '\nNFSU2_ROAM = 1\nNFSU2_ROAM = 1\nroam = on\nNFSU2_ROAM = 1\n', 'roam', 'off');
  assert.equal(two.removed, 2);
  assert.equal(S.resolveEnv(two.text, {}, {}).env.NFSU2_ROAM, '0');
  assert.ok(two.text.endsWith('roam = off\nNFSU2_ROAM = 1\n'));
  // nothing else to do: the file changes only in the row's line
  const plain = S.putKey('movies = off\nroam = on\n', 'roam', 'off');
  assert.deepEqual(plain, { text: 'movies = off\nroam = off\n', removed: 0 });
});
