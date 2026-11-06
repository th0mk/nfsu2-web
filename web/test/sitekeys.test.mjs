// This site's own controls' keys and buttons (web/src/app/sitekeys.ts):
//   node --test web/test/sitekeys.test.mjs
// - the keys and buttons from the game's environment (settings.ini key_*, button_*): the defaults, a key moved, off
//   (never the menu's), a name it does not know; the free look's switch; the key names on the caps
// - what may be picked: the game's own keys, quick chat's 1 to 4, the browser's, keys that type text for the ones
//   that work in menus too (not hiding the HUD's), another of ours; the horn and quick chat on one click
// - the keys the player put one of the game's controls on, from registry.txt's user maps (runtime/dinput_kbd.c)
// - the Site tab's rows (in a real browser, web/tools/browser.mjs): a press on a key's box takes the next key, a key
//   it may not have says why, Esc leaves it as it was, Off turns it off, a controller's press sets a button, a click
//   on the free look's cap switches it Auto / Off (no press waited for), a key the game has a control on says so;
//   the menu's copy only shows them
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { chromium } from 'playwright';
import { pickBrowser } from '../tools/browser.mjs';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const build = format => esbuild.build({ stdin: { contents: `export * from './web/src/app/sitekeys.ts';`, resolveDir: ROOT, loader: 'ts' },
  bundle: true, format, globalName: format === 'iife' ? 'K' : undefined, write: false, platform: 'neutral' }).then(r => r.outputFiles[0].text);
const K = await import('data:text/javascript;base64,' + Buffer.from(await build('esm')).toString('base64'));

test('the keys and buttons from the environment', () => {
  K.applySiteKeys({});
  assert.deepEqual(K.OURS.map(o => K.siteKey(o.id)), ['KeyX', 'KeyO', 'KeyN', 'KeyG', null, null, 'KeyI', 'KeyF', 'KeyE', 'KeyB', 'KeyV', 'KeyZ', 'F12', 'F10', 'F9', 'F7', 'F8']);
  assert.deepEqual(['button_hold', 'button_horn', 'button_chat', 'button_look'].map(K.siteButton), ['ddown', 'ls', 'rs', 'auto']);
  assert.ok(K.SITE_KEYS.includes('key_lights'), 'the headlights\' key is one of settings.ini\'s');
  assert.ok(K.SITE_KEYS.includes('key_hud') && K.SITE_KEYS.includes('button_look'), 'hiding the HUD\'s key and the free look\'s switch too');
  K.applySiteKeys({ NFSU2_BUTTON_LOOK: 'sideways' });
  assert.equal(K.siteButton('button_look'), 'auto', 'a choice it does not know: the default');
  K.applySiteKeys({ NFSU2_KEY_DRIFT: 'KeyZ', NFSU2_KEY_HORN: 'off', NFSU2_KEY_MENU: 'off', NFSU2_KEY_PHOTO: 'Nonsense',
    NFSU2_KEY_LIGHTS: 'Digit6', NFSU2_BUTTON_HOLD: 'dup', NFSU2_BUTTON_HORN: 'off', NFSU2_BUTTON_CHAT: 'ls', NFSU2_BUTTON_LOOK: 'off',
    NFSU2_KEY_HUD: 'Delete' });
  assert.equal(K.siteKey('drift'), 'KeyZ');
  assert.equal(K.siteKey('lights'), 'Digit6');
  assert.equal(K.siteKey('horn'), null, 'off');
  assert.equal(K.siteKey('menu'), 'F9', 'the menu is never off');
  assert.equal(K.siteKey('photo'), 'F12', 'a name it does not know: the default');
  assert.deepEqual(['button_hold', 'button_horn', 'button_chat', 'button_look'].map(K.siteButton), ['dup', 'off', 'ls', 'off']);
  assert.equal(K.siteKey('hud'), 'Delete');
  assert.equal(K.menuKeyOf('Delete'), null, 'hiding the HUD is the game\'s (runtime/photo.c), no key of the menus');
  assert.equal(K.menuKeyOf('F9'), 'menu');
  assert.equal(K.menuKeyOf('F12'), 'photo');
  assert.equal(K.menuKeyOf('KeyZ'), null, 'drift mode is no key of the menus');
  let n = 0;
  K.onSiteKeys(() => n++);
  K.applySiteKeys({ NFSU2_KEY_DRIFT: 'KeyZ' });
  K.applySiteKeys({ NFSU2_KEY_DRIFT: 'KeyZ' });
  assert.equal(n, 1, 'told once per change');
  K.applySiteKeys({});
  assert.deepEqual(['KeyX', 'Digit5', 'Numpad7', 'Backquote', 'F12', 'PageUp'].map(K.keyName), ['X', '5', 'Num 7', '`', 'F12', 'Page Up']);
  assert.deepEqual(['ls', 'rs'].map(v => K.buttonName(v, true)), ['L3', 'R3']);
  assert.deepEqual(['auto', 'off'].map(v => K.buttonName(v)), ['Auto', 'Off']);
});

test('what may be picked', () => {
  K.applySiteKeys({});
  assert.equal(K.refusal('drift', 'Digit7'), '');
  assert.match(K.refusal('drift', 'KeyZ'), /Z is already Invite to a race/, 'the race lobby card\'s E, V and Z');
  assert.equal(K.refusal('drift', 'KeyX'), '', 'its own key');
  assert.match(K.refusal('drift', 'KeyW'), /W is one of the game's keys/);
  assert.match(K.refusal('drift', 'KeyG'), /G is already Headlights/, 'the debug camera\'s G is free, but the headlights\'');
  assert.match(K.refusal('drift', 'KeyQ'), /Q is one of the game's keys: move its control to another key in the game's Options, Controls first/,
    'the game\'s HUD (issue 107)');
  assert.equal(K.refusal('lights', 'KeyG'), '', 'the headlights\' own G (the debug camera\'s)');
  assert.equal(K.refusal('lights', 'Digit6'), '');
  for (const code of ['KeyL', 'BracketLeft', 'BracketRight', 'Numpad6', 'Numpad7', 'Numpad8', 'Numpad9'])
    assert.equal(K.refusal('lights', code), '', `${code}: the debug camera's, which the game never reads`);
  assert.match(K.refusal('lights', 'KeyX'), /X is already Drift mode/);
  assert.match(K.refusal('lights', 'Digit3'), /quick chat's/);
  assert.equal(K.refusal('lights', 'F6'), '', 'a driving key may be an F key');
  assert.match(K.refusal('horn', 'Digit2'), /quick chat's/);
  assert.match(K.refusal('horn', 'F5'), /can't be used/);
  assert.match(K.refusal('drift', 'KeyO'), /O is already Hood, trunk, doors/);
  assert.match(K.refusal('drift', 'F10'), /F10 is already Hide HUD/);
  assert.match(K.refusal('menu', 'F10'), /F10 is already Hide HUD/);
  assert.equal(K.refusal('hud', 'Digit7'), '', 'hiding the HUD works only while driving: a key that types may do');
  assert.equal(K.refusal('hud', 'F6'), '');
  assert.match(K.refusal('hud', 'F11'), /can't be used/);
  assert.match(K.refusal('hud', 'KeyW'), /one of the game's keys/);
  assert.equal(K.buttonRefusal('button_look', 'off'), '');
  assert.match(K.refusal('menu', 'KeyZ'), /types nothing/, 'the menu works in menus: no letters');
  assert.equal(K.refusal('menu', 'F6'), '');
  assert.match(K.refusal('menu', 'F7'), /already Next song/);
  assert.equal(K.refusal('message', 'F6'), '', 'a driving key may be an F key');
  assert.equal(K.buttonRefusal('button_horn', 'ls'), '');
  assert.match(K.buttonRefusal('button_horn', 'rs'), /RS is already Quick chat/);
  assert.match(K.buttonRefusal('button_chat', 'ls'), /LS is already Horn/);
  assert.equal(K.buttonRefusal('button_chat', 'off'), '');
  assert.equal(K.buttonRefusal('button_hold', 'dleft'), '');
});

test('the keys the player put one of the game\'s controls on (registry.txt)', () => {
  const reg = ['d Version=2', 's DIMap.{25C7B2E0-6F7E-4B7B-9A0B-123456789ABC}.53.8=8100042a 00000007 00002d04',
    's DIMap.{25C7B2E0-6F7E-4B7B-9A0B-123456789ABC}.53.14=81000438 00000006 00001704',
    's DIMap.{25C7B2E0-6F7E-4B7B-9A0B-123456789ABC}.53.20=81000430 00000009 00000000'].join('\n');
  assert.deepEqual([...K.gameBoundKeys(reg)].sort(), ['KeyI', 'KeyX'], 'Shift Up on X, Nitrous on I; an action with no key none');
  assert.deepEqual([...K.gameBoundKeys(null)], []);
});

test('the driving keys the game keeps: not those whose control the player moved (registry.txt)', () => {
  const all = K.gameDrivingKeys(null);
  assert.equal(all.size, 39, 'every row but the debug camera\'s');
  for (const code of ['KeyQ', 'Equal', 'KeyW', 'ControlLeft', 'Digit5']) assert.ok(all.has(code), code);
  assert.ok(!all.has('KeyL') && !all.has('KeyG'), 'the debug camera\'s');
  const reg = ['d Version=2', 's DIMap.{25C7B2E0-6F7E-4B7B-9A0B-123456789ABC}.53.16=81000410 0000000b 00002c04',   // HUD: Q -> Z
    's DIMap.{25C7B2E0-6F7E-4B7B-9A0B-123456789ABC}.53.21=81000425 00000009 00000000',                              // Look Back's K: none
    's DIMap.{25C7B2E0-6F7E-4B7B-9A0B-123456789ABC}.53.1=81000411 00000001 00001104',                               // Throttle's W: W still
    's DIMap.{0A1B2C3D-0000-0000-0000-000000000000}.32.16=81000410 00000004 00002c04'].join('\n');                  // another map's row 16
  const kept = K.gameDrivingKeys(reg);
  assert.ok(!kept.has('KeyQ') && !kept.has('KeyK'), 'moved off, or left with no key');
  assert.ok(kept.has('KeyW') && kept.has('Equal'), 'still on its default; HUD\'s other key');
  assert.equal(K.refusal('drift', 'KeyQ', K.siteKey, kept), '', 'Q free once HUD has another key');
  assert.match(K.refusal('drift', 'KeyW', K.siteKey, kept), /W is one of the game's keys/);
});

test('the Site tab\'s rows', async () => {
  const browser = await chromium.launch({ ...pickBrowser(), headless: true });
  try {
    const p = await browser.newPage();
    await p.setContent('<div id="box"></div><div id="copy"></div>');
    await p.addScriptTag({ content: await build('iife') });
    await p.evaluate(() => {
      const w = /** @type {any} */ (window);
      w.file = {}; w.reg = 's DIMap.{X}.53.14=81000438 00000006 00002104\n'     // Nitrous on F
        + 's DIMap.{X}.53.8=8100042a 00000007 00002204\n';                      // Shift Up on G
      w.pads = [];
      navigator.getGamepads = () => w.pads;
      w.deps = { set: (k, v) => { if (v === null) delete w.file[k]; else w.file[k] = v; }, current: k => w.file[k] ?? '',
        readOnly: () => false, registry: async () => w.reg, ps: () => false };
      w.view = new w.K.SiteKeysView(w.deps);
      w.view.render(document.getElementById('box'), true);
    });
    const row = id => p.evaluate(i => { const r = document.querySelector(`#box [data-our="${i}"]`);
      return { caps: [...r.querySelectorAll('dt > button, dt > kbd')].map(b => b.textContent || b.querySelector('svg')?.getAttribute('aria-label')), state: r.querySelector('.set-state')?.textContent ?? '' }; }, id);
    await p.waitForFunction(() => document.querySelector('#box [data-our="friend"] .set-state'));
    assert.deepEqual(await row('drift'), { caps: ['X', 'D-pad ↓'], state: '' });
    assert.deepEqual(await row('chat'), { caps: ['1–4', 'RS'], state: '' });
    assert.deepEqual(await row('hud'), { caps: ['F10'], state: '' });
    // the free look: a switch, a click its next choice (no press waited for); back to the default: no line
    assert.deepEqual(await row('look'), { caps: ['Auto'], state: '' });
    await p.click('#box [data-our="look"] dt > button');
    assert.equal(await p.evaluate(() => /** @type {any} */ (window).file.button_look), 'off');
    assert.deepEqual(await row('look'), { caps: ['Off'], state: '' });
    await p.click('#box [data-our="look"] dt > button');
    assert.equal(await p.evaluate(() => 'button_look' in /** @type {any} */ (window).file), false, 'Auto, the default: no line');
    assert.deepEqual(await row('look'), { caps: ['Auto'], state: '' });
    await p.click('#box [data-our="look"] dt > button');
    assert.match((await row('friend')).state, /One of the game's controls is on F/, 'the game has F');
    const lights = await row('lights');
    assert.deepEqual(lights.caps, ['G'], 'the headlights: a key, no button (the look\'s stick up)');
    assert.match(lights.state, /One of the game's controls is on G/, 'the game has G');
    // a key: the next press
    await p.click('#box [data-our="drift"] dt > button >> nth=0');
    assert.match((await row('drift')).caps[0], /Press a key/);
    await p.keyboard.press('KeyW');
    assert.match((await row('drift')).state, /W is one of the game's keys/);
    assert.equal(await p.evaluate(() => /** @type {any} */ (window).file.key_drift), undefined, 'not taken');
    await p.click('#box [data-our="drift"] dt > button >> nth=0');
    await p.keyboard.press('Digit7');
    assert.equal(await p.evaluate(() => /** @type {any} */ (window).file.key_drift), 'Digit7');
    assert.deepEqual((await row('drift')).caps, ['7', 'D-pad ↓']);
    // Esc: as it was; Off; back to the default: the line goes
    await p.click('#box [data-our="drift"] dt > button >> nth=0');
    await p.keyboard.press('Escape');
    assert.deepEqual((await row('drift')).caps, ['7', 'D-pad ↓']);
    await p.click('#box [data-our="drift"] dt > button >> nth=0');
    await p.click('#box [data-our="drift"] .set-state .text-btn');
    assert.equal(await p.evaluate(() => /** @type {any} */ (window).file.key_drift), 'off');
    assert.deepEqual((await row('drift')).caps, ['Off', 'D-pad ↓']);
    await p.click('#box [data-our="drift"] dt > button >> nth=0');
    await p.keyboard.press('KeyX');
    assert.equal(await p.evaluate(() => 'key_drift' in /** @type {any} */ (window).file), false, 'the default: no line');
    // the menu has no Off
    await p.click('#box [data-our="menu"] dt > button');
    assert.equal(await p.evaluate(() => !!document.querySelector('#box [data-our="menu"] .set-state .text-btn')), false);
    await p.keyboard.press('Escape');
    // a button: a controller's press of one it offers
    await p.click('#box [data-our="horn"] dt > button >> nth=1');
    assert.match((await row('horn')).caps[1], /Press a button/);
    const press = i => p.evaluate(k => { const w = /** @type {any} */ (window);
      w.pads = [{ connected: true, buttons: Array.from({ length: 17 }, (_, j) => ({ pressed: j === k, value: j === k ? 1 : 0 })) }]; }, i);
    await press(11);                                       // RS: quick chat's
    await p.waitForFunction(() => /already/.test(document.querySelector('#box [data-our="horn"] .set-state')?.textContent ?? ''));
    await p.click('#box [data-our="horn"] dt > button >> nth=1');
    await press(-1);
    await p.waitForTimeout(50);
    await press(13);                                       // D-pad down: not one of the horn's
    await p.waitForTimeout(100);
    assert.equal(await p.evaluate(() => /** @type {any} */ (window).file.button_horn), undefined);
    await press(-1);
    await p.click('#box [data-our="horn"] .set-state .text-btn');   // Off
    assert.equal(await p.evaluate(() => /** @type {any} */ (window).file.button_horn), 'off');
    // the menu's copy: caps only
    await p.evaluate(() => { const w = /** @type {any} */ (window); new w.K.SiteKeysView(w.deps).render(document.getElementById('copy'), false); });
    assert.equal(await p.evaluate(() => document.querySelectorAll('#copy button').length), 0);
    assert.deepEqual(await p.evaluate(() => [...document.querySelectorAll('#copy [data-our="horn"] kbd')].map(k => k.textContent)), ['N', 'Off']);
    assert.deepEqual(await p.evaluate(() => [...document.querySelectorAll('#copy [data-our="look"] kbd')].map(k => k.textContent)), ['Off']);
  } finally { await browser.close(); }
});
