// The game's and the page's language for a player who has not picked one (web/src/app/i18n.ts browserLanguage):
//   node --test web/test/browser-language.test.mjs
// 1. The browser's languages, most wanted first, read by their language subtag: the first the game has, else English.
//    A language the game lacks is passed over (Azerbaijani is not Turkish), never matched to one near it.
// 2. Below settings.ini and the page address (shell.ts DEVICE, settings.ts resolveEnv's device defaults): a language
//    the player picked stays, English too, and the language row's Reset gives the browser's back.
// 3. The page before settings.ini is read (lastLanguage, boot.ts): the language shown last, on a first visit the
//    browser's.
// The modules are bundled from web/src with esbuild, as web/test/settings-reset.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * from './web/src/app/settings.ts'; export * as I from './web/src/app/i18n.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const S = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
const { browserLanguage, lastLanguage, LANGUAGES, LOCALE } = S.I;

test('the browser\'s first language the game has', () => {
  for (const [tags, want] of [
    [['tr-TR', 'tr', 'en-US', 'en'], 'turkish'],          // Chrome in Turkish
    [['hu-HU', 'hu', 'en-US', 'en'], 'hungarian'],
    [['de'], 'german'],
    [['en-GB'], 'english'],
    [['ru-UA', 'uk'], 'russian'],                          // the order is the player's
    [['uk', 'ru'], 'ukrainian'],
    [['pt-PT'], 'portuguese'],                             // the game's is Brazil's: the same language
    [['zh-TW'], 'chinese'], [['zh-Hant-HK'], 'chinese'],
    [['zh-CN'], 'chinese'],                                // its only Chinese, Traditional, before English
    [['TR_tr'], 'turkish'], [[' el-GR '], 'greek'],      // as a system's locale may spell it
  ]) assert.equal(browserLanguage(tags), want, tags.join(', '));
});

test('a language the game does not have: the browser\'s next, else English', () => {
  for (const [tags, want] of [
    [['az-AZ', 'az'], 'english'],                          // Azerbaijani: English, not the Turkish near it
    [['az', 'en-US', 'en', 'tr'], 'english'],              // English before Turkish in the browser's list
    [['az', 'tr'], 'turkish'],                             // Turkish next in the browser's list: the player reads it
    [['nb-NO', 'nb'], 'english'], [['nb-NO', 'da'], 'danish'],
    [['ar-SA'], 'english'], [['ka'], 'english'],
    [[], 'english'], [['*'], 'english'], [[''], 'english'], [['und'], 'english'],
  ]) assert.equal(browserLanguage(tags), want, tags.join(', ') || '(none)');
});

test('every game language, by its own tag', () => {
  for (const l of LANGUAGES) assert.equal(browserLanguage([LOCALE[l]]), l, LOCALE[l]);
});

test('no language the browser gives: English (the default navigator.languages, read when called)', () => {
  const was = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
  const as = nav => Object.defineProperty(globalThis, 'navigator', { value: nav, configurable: true, writable: true });
  try {
    as({ languages: ['hu-HU', 'hu'], language: 'hu-HU' });
    assert.equal(browserLanguage(), 'hungarian');
    as({ languages: [], language: 'tr' });                 // a browser with the one language only
    assert.equal(browserLanguage(), 'turkish');
    as({});
    assert.equal(browserLanguage(), 'english');
  } finally {
    if (was) Object.defineProperty(globalThis, 'navigator', was); else delete globalThis.navigator;
  }
});

test('below settings.ini and the page address: the player\'s pick stays, Reset gives the browser\'s back', () => {
  const device = { language: browserLanguage(['tr-TR', 'tr']) };
  const lang = (text, env = {}) => S.resolveEnv(text, env, {}, device);
  // a new player, and one whose file is older than this (the template's language line commented out): the browser's
  assert.equal(lang(S.TEMPLATE).env.NFSU2_LANGUAGE, 'turkish');
  assert.equal(lang(S.TEMPLATE).source.NFSU2_LANGUAGE, 'device');
  assert.equal(lang(null).env.NFSU2_LANGUAGE, 'turkish', 'settings.ini not read yet');
  // picked in Settings or the list at the top (settings-ui.ts put: settingsPut), English as well
  const english = S.settingsPut(S.TEMPLATE, 'language', 'english');
  assert.equal(lang(english).env.NFSU2_LANGUAGE, 'english');
  assert.equal(lang(english).source.NFSU2_LANGUAGE, 'settings.ini');
  assert.equal(lang(S.settingsPut(S.TEMPLATE, 'language', 'hungarian')).env.NFSU2_LANGUAGE, 'hungarian');
  // the page address on a development server, over both
  assert.equal(lang(english, { NFSU2_LANGUAGE: 'german' }).env.NFSU2_LANGUAGE, 'german');
  // the row's Reset (settingsPut null: the template's commented line again): the browser's language
  assert.equal(S.settingsPut(english, 'language', null), S.TEMPLATE);
  assert.equal(lang(S.settingsPut(english, 'language', null)).env.NFSU2_LANGUAGE, 'turkish');
  // a language the browser's list does not have in the game: English, as before
  assert.equal(S.resolveEnv(S.TEMPLATE, {}, {}, { language: browserLanguage(['az']) }).env.NFSU2_LANGUAGE, 'english');
  // not a setting the player changed (the Play tally's changedSettings), however it came
  assert.deepEqual(S.changedSettings(lang(S.TEMPLATE), device).filter(s => s.startsWith('language')), []);
});

test('before settings.ini is read: the language shown last, on a first visit the browser\'s', () => {
  const had = { navigator: Object.getOwnPropertyDescriptor(globalThis, 'navigator'), localStorage: Object.getOwnPropertyDescriptor(globalThis, 'localStorage') };
  const put = (k, value) => Object.defineProperty(globalThis, k, { value, configurable: true, writable: true });
  const store = new Map();
  try {
    put('navigator', { languages: ['tr-TR', 'tr'] });
    put('localStorage', { getItem: k => store.get(k) ?? null, setItem: (k, v) => store.set(k, String(v)) });
    assert.equal(lastLanguage(), 'turkish', 'a first visit');
    store.set('nfsu2-page-language', 'english');
    assert.equal(lastLanguage(), 'english', 'English shown last (picked)');
    store.set('nfsu2-page-language', 'klingon');
    assert.equal(lastLanguage(), 'turkish', 'not a language');
    put('localStorage', { getItem() { throw new Error('SecurityError'); } });
    assert.equal(lastLanguage(), 'turkish', 'no storage');
  } finally {
    for (const [k, d] of Object.entries(had)) { if (d) Object.defineProperty(globalThis, k, d); else delete globalThis[k]; }
  }
});
