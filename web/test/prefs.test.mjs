// The page's way into localStorage and sessionStorage (web/src/app/prefs.ts): it never throws, with no store, a store
// that refuses, or one that is full; and the keys are spelled as players' kept choices are (a key changed loses them).
//   node --test web/test/prefs.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const P = await import('data:text/javascript;base64,' + Buffer.from((await esbuild.build({
  entryPoints: [path.join(ROOT, 'web/src/app/prefs.ts')], bundle: true, format: 'esm', write: false })).outputFiles[0].text).toString('base64'));

const mapStore = (full = false) => {
  const data = new Map();
  return { data, getItem: k => data.get(k) ?? null, setItem: (k, v) => { if (full) throw new Error('QuotaExceededError'); data.set(k, String(v)); },
    removeItem: k => data.delete(k) };
};
const refusing = { getItem() { throw new Error('SecurityError'); }, setItem() { throw new Error('SecurityError'); }, removeItem() { throw new Error('SecurityError'); } };
const put = (name, s) => Object.defineProperty(globalThis, name, { value: s, configurable: true, writable: true });

test('a store that works: read, write, JSON, remove', () => {
  const s = mapStore();
  put('localStorage', s);
  assert.equal(P.localStore.usable(), true);
  assert.equal(P.localStore.get('a'), null);
  assert.equal(P.localStore.set('a', '1'), true);
  assert.equal(P.localStore.get('a'), '1');
  assert.equal(P.localStore.setJSON('j', { x: [1, 2] }), true);
  assert.equal(s.data.get('j'), '{"x":[1,2]}');
  assert.deepEqual(P.localStore.getJSON('j'), { x: [1, 2] });
  s.data.set('bad', '{not json');
  assert.equal(P.localStore.getJSON('bad'), null);
  assert.equal(P.localStore.getJSON('none'), null);
  P.localStore.remove('a');
  assert.equal(P.localStore.get('a'), null);
});

test('no store, or one that refuses: null, false, nothing thrown; failed hears why', () => {
  for (const s of [undefined, refusing]) {
    if (s) put('sessionStorage', s); else delete globalThis.sessionStorage;
    const why = [];
    assert.equal(P.sessionStore.usable(), false);
    assert.equal(P.sessionStore.get('a'), null);
    assert.equal(P.sessionStore.getJSON('a'), null);
    assert.equal(P.sessionStore.set('a', '1', e => why.push(e)), false);
    assert.equal(P.sessionStore.setJSON('a', [1], e => why.push(e)), false);
    assert.doesNotThrow(() => P.sessionStore.remove('a'));
    assert.equal(why.length, 2);
  }
});

test('a full store: reads go on, writes say not kept; a value with no JSON is not kept either', () => {
  const s = mapStore(true);
  s.data.set('k', 'v');
  put('localStorage', s);
  let why = '';
  assert.equal(P.localStore.usable(), true);
  assert.equal(P.localStore.get('k'), 'v');
  assert.equal(P.localStore.set('k', 'w', e => { why = String(e); }), false);
  assert.match(why, /QuotaExceededError/);
  put('localStorage', mapStore());
  assert.equal(P.localStore.setJSON('n', 1n, e => { why = String(e); }), false);
  assert.match(why, /BigInt/);
});

test('the store is looked up at each call (blocked later, or a test\'s own)', () => {
  put('localStorage', mapStore());
  P.localStore.set('a', '1');
  put('localStorage', refusing);
  assert.equal(P.localStore.get('a'), null);
  put('localStorage', mapStore());
  assert.equal(P.localStore.get('a'), null);
});

test('the keys, as players\' browsers keep them', () => {
  const keys = Object.fromEntries(Object.entries(P).filter(([, v]) => typeof v === 'string'));
  assert.deepEqual(keys, {
    PAGE_LANGUAGE: 'nfsu2-page-language', PAGE_TAB: 'nfsu2.page.tab', PAGE_ID: 'nfsu2.net.page-id',
    SEEN_SETTINGS: 'nfsu2.seen.settings', LOG_SALT: 'nfsu2.logsalt', DEVICE_RENDER: 'nfsu2.device.render',
    GPU_PIPELINES: 'nfsu2.gpu.pipelines.v1', LAST_GAME: 'nfsu2.lastgame.',
    PLAY_MS: 'nfsu2.play.ms', PLAY_SMS: 'nfsu2.play.sms', PLAY_GAMES: 'nfsu2.play.games', PLAY_LAST: 'nfsu2.play.last',
    PLAY_FROM: 'nfsu2.play.from',
    RADIO_MODES: 'nfsu2.radio.modes', RADIO_ON: 'nfsu2.radio.on', RADIO_NEXT: 'nfsu2.radio.next', RADIO_SHUFFLE: 'nfsu2.radio.shuffle',
    LASTFM_SESSION: 'nfsu2.lastfm', LASTFM_QUEUE: 'nfsu2.lastfm.queue', LASTFM_RESULT: 'nfsu2.lastfm.result',
    PHOTO_KEPT: 'nfsu2-photo', PHOTO_MARK: 'nfsu2-photo-mark', MESSAGES: 'nfsu2.msgs.',
    TOUCH_STEERING: 'nfsu2-touch-steering', TOUCH_LEVEL: 'nfsu2-touch-level', TOUCH_NITRO: 'nfsu2-touch-nitro',
    TOUCH_POSITION: 'nfsu2-touch-position', TOUCH_SIZE: 'nfsu2-touch-size', MENU_SIZE: 'nfsu2-menu-size',
    WHEEL: 'nfsu2.wheel.', SHIFTER: 'nfsu2.shifter.', ALTGR_CTRL: 'nfsu2.altgr-ctrl',
    KEYPHRASE: 'nfsu2-keyphrase:', CLOUD_OFFER: 'nfsu2-cloud-offer:', KEYPHRASE_KEPT: 'nfsu2-keyphrase-kept:',
    NAME_ASKED: 'nfsu2-name-asked:', NAME_CLAIMED: 'nfsu2-name-claimed:', CLAIM_TRIED: 'nfsu2-claim-tried:',
    OTHER_BUILD: 'nfsu2-other-build', UPDATE_FOR: 'nfsu2-update',
  });
});
