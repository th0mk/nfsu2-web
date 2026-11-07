// Windows' AltGr (web/src/input/altgr.ts, issue 58): its Left Ctrl kept from the game once the browser has shown that
// it sends one, every other Left Ctrl sent (at most WAIT_MS later), and nothing changed off Windows.
//   node --test web/test/altgr.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const A = await import('data:text/javascript;base64,' + Buffer.from((await esbuild.build({
  entryPoints: [path.join(ROOT, 'web/src/input/altgr.ts')], bundle: true, format: 'esm', write: false })).outputFiles[0].text).toString('base64'));
const sleep = ms => new Promise(r => setTimeout(r, ms));

// what Chrome on Windows sends for AltGr: Left Ctrl, then Right Alt as AltGraph (and their releases)
const CTRL = { code: 'ControlLeft', key: 'Control', repeat: false };
const ALTGR = { code: 'AltRight', key: 'AltGraph', repeat: false };
const ALT = { code: 'AltRight', key: 'Alt', repeat: false };
const W = { code: 'KeyW', key: 'w', repeat: false };

function rig({ on = true, stored = null } = {}) {
  let t = 1000;
  const sent = [], logs = [], data = new Map(stored ? [['nfsu2.altgr-ctrl', stored]] : []);
  const store = { getItem: k => data.get(k) ?? null, setItem: (k, v) => data.set(k, v) };
  const f = new A.AltGrCtrl(on, s => logs.push(s), () => t, store);
  const key = (e, down, dt = 0) => { t += dt; f.key(e, down, () => sent.push(`${down ? '+' : '-'}${e.code}`)); };
  return { f, key, sent, logs, data };
}

test('off Windows nothing is held or dropped', () => {
  const r = rig({ on: false, stored: '1' });
  r.key(CTRL, true); r.key(ALTGR, true, 0.2); r.key(CTRL, false); r.key(ALTGR, false);
  assert.deepEqual(r.sent, ['+ControlLeft', '+AltRight', '-ControlLeft', '-AltRight']);
});

test('the first AltGr goes through as it did, and teaches the browser', () => {
  const r = rig();
  r.key(CTRL, true); r.key(ALTGR, true, 0.3);
  assert.deepEqual(r.sent, ['+ControlLeft', '+AltRight'], 'nothing told it yet');
  assert.equal(r.data.get('nfsu2.altgr-ctrl'), '1');
  assert.match(r.logs.join('\n'), /sends AltGr as Left Ctrl and Right Alt/);
  r.key(CTRL, false); r.key(ALTGR, false);
  r.key(CTRL, true, 500); r.key(ALTGR, true, 0.3);
  assert.deepEqual(r.sent.slice(4), ['+AltRight'], 'the next one: its Left Ctrl dropped');
  assert.match(r.logs.join('\n'), /its Left Ctrl \(0\.3 ms before Right Alt\) does not reach the game/);
});

test('a Right Alt without AltGr (US layout), or long after a Left Ctrl, teaches nothing', () => {
  const r = rig();
  r.key(CTRL, true); r.key(ALT, true, 0.3);
  r.key(CTRL, false); r.key(ALT, false);
  r.key(CTRL, true, 100); r.key(ALTGR, true, 60);
  assert.equal(r.data.get('nfsu2.altgr-ctrl'), undefined);
  assert.equal(r.sent.filter(s => s === '+ControlLeft').length, 2);
});

test('once learnt: Left Ctrl alone still reaches the game, within WAIT_MS', async () => {
  const r = rig({ stored: '1' });
  r.key(CTRL, true);
  assert.deepEqual(r.sent, [], 'held for a moment');
  await sleep(A.WAIT_MS + 20);
  assert.deepEqual(r.sent, ['+ControlLeft']);
  r.key(CTRL, false);
  assert.deepEqual(r.sent, ['+ControlLeft', '-ControlLeft']);
});

test('once learnt: another key, or Left Ctrl\'s own release, sends the held press first', () => {
  const r = rig({ stored: '1' });
  r.key(CTRL, true); r.key(W, true, 2);
  assert.deepEqual(r.sent, ['+ControlLeft', '+KeyW']);
  r.key(W, false); r.key(CTRL, false);
  r.key(CTRL, true, 100); r.key(CTRL, false, 5);
  assert.deepEqual(r.sent.slice(4), ['+ControlLeft', '-ControlLeft'], 'a quick tap: down, then up');
});

test('AltGr held: the dropped Left Ctrl\'s repeats are dropped too, until its release', () => {
  const r = rig({ stored: '1' });
  r.key(CTRL, true); r.key(ALTGR, true, 0.2);
  r.key({ ...CTRL, repeat: true }, true, 30); r.key({ ...ALTGR, repeat: true }, true, 30);
  r.key(CTRL, false); r.key(ALTGR, false);
  assert.deepEqual(r.sent, ['+AltRight', '+AltRight', '-ControlLeft', '-AltRight']);
  r.key(CTRL, true, 100); r.key({ ...CTRL, repeat: true }, true, 30);
  assert.deepEqual(r.sent.slice(4), ['+ControlLeft', '+ControlLeft'], 'a real Left Ctrl held: its repeats go on');
});

test('cancel drops a held Left Ctrl', async () => {
  const r = rig({ stored: '1' });
  r.key(CTRL, true); r.f.cancel();
  await sleep(A.WAIT_MS + 20);
  assert.deepEqual(r.sent, []);
});

test('no storage: learnt for the tab only', () => {
  const sent = [];
  const f = new A.AltGrCtrl(true, () => {}, () => 0, { getItem() { throw new Error('denied'); }, setItem() { throw new Error('denied'); } });
  const key = (e, down) => f.key(e, down, () => sent.push(`${down ? '+' : '-'}${e.code}`));
  key(CTRL, true); key(ALTGR, true); key(CTRL, false); key(ALTGR, false);
  key(CTRL, true); key(ALTGR, true);
  assert.deepEqual(sent, ['+ControlLeft', '+AltRight', '-ControlLeft', '-AltRight', '+AltRight']);
});
