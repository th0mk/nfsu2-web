// The settings a day's first Play tells the server (web/server-rs src/app/settings_days.rs):
//   node --test web/test/settings-report.test.mjs
// - changedSettings (web/src/app/settings.ts): only what settings.ini changes from this device's defaults, a switch
//   as on/off, choices and numbers as they apply, the player's own words only as "set", nothing of the Mac's keys,
//   the LAN's names and addresses, variables outside the table or the page address
// - seen (web/src/app/page-id.ts): the report goes with a Play once a UTC day, never with a load, and a day whose
//   request failed is told again
// The modules are bundled from web/src with esbuild, as web/test/settings-reset.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const bundle = async entry => {
  const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
  return import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
};
const S = await bundle(`export * from './web/src/app/settings.ts';`);
const P = await bundle(`export * from './web/src/app/page-id.ts';`);

const ini = (pairs, extra = '') => pairs.reduce((t, [k, v]) => S.settingsPut(t, k, v), S.TEMPLATE) + extra;
const changed = (text, device = {}, environment = {}) => S.changedSettings(S.resolveEnv(text, environment, {}, device), device);

test('the template changes nothing, nor do values at their defaults', () => {
  assert.deepEqual(changed(S.TEMPLATE), []);
  assert.deepEqual(changed(null), []);
  const d = S.DEFAULTS;
  assert.deepEqual(changed(ini([['fps', d.get('fps')], ['movies', 'on'], ['hud', d.get('hud')], ['roam_max_cars', d.get('roam_max_cars')]])), []);
});

test('switches, choices, numbers and sizes as they apply; the player\'s words as "set"', () => {
  const out = changed(ini([
    ['fps', '30'], ['movies', 'off'], ['rumble', 'off'], ['render', 'SCALE:0.75'], ['resolution', '1920X1080'],
    ['gamepad_deadzone', '0.150'], ['roam_max_cars', '40'], ['roam_name', 'Lessis'], ['roam_room', 'Night Crew'],
    ['roam_crew', 'Bayview Kings'], ['roam_block', 'someone'], ['language', 'russian'],
  ]));
  assert.deepEqual(out.sort(), [
    'fps=30', 'gamepad_deadzone=0.15', 'language=russian', 'movies=off', 'render=scale:0.75', 'resolution=1920x1080',
    'roam_block=set', 'roam_crew=set', 'roam_max_cars=40', 'roam_name=set', 'roam_room=set', 'rumble=off',
  ]);
  for (const s of out) assert.match(s, /^[a-z0-9_]{1,32}=[A-Za-z0-9:._+-]{1,24}$/, 'as the server takes them');
  assert.ok(!out.join().includes('Lessis') && !out.join().includes('Night') && !out.join().includes('Bayview'));
});

test('the device\'s own defaults: a phone at 75% changed nothing, a computer at 75% did', () => {
  const text = ini([['render', 'scale:0.75'], ['hud', '43'], ['roam_max_cars', '16']]);
  const phone = { render: 'scale:0.75', fullscreen: 'on', hud: '43', roam_max_cars: '16' };
  assert.deepEqual(changed(text, phone), []);
  assert.deepEqual(changed(text).sort(), ['hud=43', 'render=scale:0.75', 'roam_max_cars=16']);
  assert.deepEqual(changed(ini([['fullscreen', 'off']]), phone), ['fullscreen=off']);
  assert.deepEqual(changed(ini([['roam_max_cars', '24']]), { roam_max_cars: '24' }), [], 'a tablet\'s 24');
});

test('left out: the Mac\'s keys, the LAN\'s names, variables outside the table, the page address', () => {
  const text = ini([['graphics', 'enhanced'], ['msaa', '4'], ['lan_host_name', 'my-pc'], ['lan_adapter_id', '02:00:00:00:00:01']],
    'NFSU2_STATUS_EVERY = 600\nNFSU2_FPS = 120\n');
  assert.deepEqual(changed(text), ['fps=120'], 'a setting by its variable\'s name counts as the setting');
  assert.deepEqual(changed(ini([['fps', '30']]), {}, { NFSU2_FPS: '60' }), [], 'the page address wins and is not told');
  assert.deepEqual(changed(ini([['fps', 'sixty']])), [], 'a refused value is not applied');
});

test('seen: the report goes with a day\'s first Play only', async () => {
  const store = new Map();
  globalThis.localStorage = { getItem: k => store.get(k) ?? null, setItem: (k, v) => store.set(k, String(v)), removeItem: k => store.delete(k) };
  const urls = [];
  let answer = () => Promise.resolve({ ok: true });
  globalThis.fetch = url => { urls.push(url); return answer(); };
  const settle = () => new Promise(r => setTimeout(r, 20));
  const report = { device: 'phone', changed: ['fps=30', 'roam_name=set'] };
  const today = new Date().toISOString().slice(0, 10);

  P.seen('play', report); await settle();
  assert.match(urls[0], /^\/seen\?at=play&id=[A-Za-z0-9_-]{22}&k=phone&s=fps%3D30%2Croam_name%3Dset$/);
  assert.equal(store.get('nfsu2.seen.settings'), today);
  P.seen('play', report); await settle();
  assert.match(urls[1], /^\/seen\?at=play&id=[A-Za-z0-9_-]{22}$/, 'told today already');
  P.seen('page', report); await settle();
  assert.match(urls[2], /^\/seen\?at=page&id=[A-Za-z0-9_-]{22}$/, 'never with a load');

  store.set('nfsu2.seen.settings', '2000-01-01');
  answer = () => Promise.reject(new Error('offline'));
  P.seen('play', { device: 'pc', changed: [] }); await settle();
  assert.match(urls[3], /&k=pc&s=$/, 'a new day: told, with nothing changed');
  assert.equal(store.get('nfsu2.seen.settings'), '2000-01-01', 'the request failed: not marked');
  answer = () => Promise.resolve({ ok: false });
  P.seen('play', report); await settle();
  assert.match(urls[4], /&k=phone&s=/);
  assert.equal(store.get('nfsu2.seen.settings'), '2000-01-01', 'an error answer: not marked');
  answer = () => Promise.resolve({ ok: true });
  P.seen('play', report); await settle();
  assert.equal(store.get('nfsu2.seen.settings'), today);
});
