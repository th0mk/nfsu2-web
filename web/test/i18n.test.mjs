// The start page's languages (web/src/app/i18n.ts):
//   node --test web/test/i18n.test.mjs
// 1. Every text the page shows is one the extraction reads (web/tools/i18n-keys.mjs): N() and tn() of literals only.
// 2. Every language's file (web/src/app/lang/<language>.json) has every text in use and nothing else, each with the
//    English's {name}s (a plural's one form may say {n} in words) and <n> tags, plural forms of its own language.
//    A new or changed English text fails here until each language has it: `node web/tools/i18n-keys.mjs <language>`
//    lists what one lacks, `node web/tools/i18n-keys.mjs --check` what is wrong.
//    Both for each game the page is built for (web/src/games/<id>, web/build.mjs --game): a game's texts are the
//    shared sources' and its own folder's, its files the shared ones with its own (web/src/games/<id>/lang) over them,
//    and its own alone for a language only it has (Most Wanted's Finnish); its languages are its files' and its list's
//    (web/src/games/<id>/languages.ts).
// 3. The same rules on a made-up page of two games: a text only one game uses, a game's own wording of a shared text,
//    what is wrong when a text sits in the wrong file, and a language only one game has.
// 4. t(), tn() and tvars() as the page calls them: English as written, a language's text and plural forms once its
//    file is in (fetched, as the page fetches /lang/<language>.json), English for a text the file lacks.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';
import { catalog, check, GAMES, gamesIn, langFile, langTexts, languageProblems, languages, LANGUAGE_FILES, ownLangFile } from '../tools/i18n-keys.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));

test('the games: ug2 and mw', () => assert.deepEqual(GAMES, ['ug2', 'mw']));

for (const game of GAMES) {
  const { keys, bad } = catalog(game);

  test(`${game}: the texts in use are literals the extraction reads`, () => {
    assert.deepEqual(bad, []);
    assert.ok(keys.size > 500, `${keys.size} texts`);
    assert.ok(keys.has('Fan project. Not affiliated with or endorsed by Electronic Arts.'), 'index.html data-i18n');
    assert.ok(keys.has('Post the log in <0>#bug-reports on our Discord</0>, with what you were doing.'), 'index.html data-i18n-html');
    assert.ok(keys.get('{n} laps')?.plural, 'tn()');
    for (const [k, e] of keys) for (const w of e.where) assert.ok(!w.startsWith('games/') || w.startsWith(`games/${game}/`), `${JSON.stringify(k)} from ${w}`);
  });

  // the page's languages: the files' (the shared ones, every game's, and the game's own of a language only it has), as
  // the game lists them (src/games/<id>/languages.ts: the picker, settings.ini's values) and as its page has them
  test(`${game}: its languages, the files' and the list's`, async () => {
    assert.deepEqual(languageProblems(game), []);
    for (const l of LANGUAGE_FILES) assert.ok(languages(game).includes(l), l);
    const out = await esbuild.build({ entryPoints: [path.join(HERE, '../src/app/i18n.ts')], bundle: true, write: false, format: 'esm', platform: 'neutral',
      alias: { '@game': path.join(HERE, '../src/games', game) } });
    const I = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));
    assert.deepEqual([...I.LANGUAGES].filter(l => l !== 'english').sort(), [...languages(game)].sort());
    for (const l of I.LANGUAGES) assert.ok(I.LOCALE[l] && I.NATIVE[l], l);
  });

  for (const lang of languages(game)) {
    test(`${game} ${lang}: every text, as the English has it`, () => {
      assert.ok(fs.existsSync(langFile(lang)) || fs.existsSync(ownLangFile(game, lang)), `${lang}.json`);
      assert.deepEqual(check(lang, keys, game), []);
    });
  }
}

test('Finnish: Most Wanted\'s page only, every text in its own file', () => {
  assert.ok(!fs.existsSync(langFile('finnish')), 'no shared file: Underground 2\'s page has no Finnish');
  assert.ok(!languages('ug2').includes('finnish'));
  assert.ok(languages('mw').includes('finnish'));
  assert.deepEqual(Object.keys(langTexts('mw', 'finnish')).sort(), [...catalog('mw').keys.keys()].sort());
});

test('a page of two games: shared texts, each game\'s own, and texts in the wrong file', t => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'i18n-games-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const w = (f, s) => { fs.mkdirSync(path.dirname(path.join(root, f)), { recursive: true }); fs.writeFileSync(path.join(root, f), typeof s === 'string' ? s : JSON.stringify(s)); };
  w('index.html', '<p data-i18n>Shared markup</p>');
  w('src/app/a.ts', 'export const a = () => t(\'Shared text\');');
  w('src/test/x.ts', 't(\'Test page text\');');
  w('src/games/one/index.ts', 'export const b = () => t(\'Only one\');');
  w('src/games/two/index.ts', 'export const c = () => t(\'Only two\');');
  w('src/games/two/ui/notes.ts', 't(\'Two again\');');
  w('src/app/lang/french.json', { 'Shared markup': 'Balisage partagé', 'Shared text': 'Texte partagé' });
  w('src/games/one/lang/french.json', { 'Only one': 'Seulement un', 'Shared text': 'Texte partagé, à la une' });
  assert.deepEqual(gamesIn(root), ['one', 'two']);
  assert.deepEqual([...catalog('one', root).keys.keys()].sort(), ['Only one', 'Shared markup', 'Shared text']);
  assert.deepEqual([...catalog('two', root).keys.keys()].sort(), ['Only two', 'Shared markup', 'Shared text', 'Two again']);
  assert.deepEqual(check('french', null, 'one', root), []);
  assert.deepEqual(langTexts('one', 'french', root), { 'Shared markup': 'Balisage partagé', 'Shared text': 'Texte partagé, à la une', 'Only one': 'Seulement un' });
  // two's own texts go in its own file
  assert.deepEqual(check('french', null, 'two', root), ['missing (games/two/lang/french.json): "Only two"', 'missing (games/two/lang/french.json): "Two again"']);
  w('src/games/two/lang/french.json', { 'Only two': 'Seulement deux', 'Two again': 'Encore deux', 'Shared markup': 'Balisage partagé' });
  assert.deepEqual(check('french', null, 'two', root), ['games/two/lang/french.json says it as the shared file does: "Shared markup"']);
  // one's own text in the shared file, and in two's file: not in use on two's page
  w('src/app/lang/french.json', { 'Shared markup': 'Balisage partagé', 'Shared text': 'Texte partagé', 'Only one': 'Seulement un' });
  w('src/games/two/lang/french.json', { 'Only two': 'Seulement deux', 'Two again': 'Encore deux', 'Only one': 'Un seul' });
  assert.deepEqual(check('french', null, 'two', root), ['not in use (games/two/lang/french.json): "Only one"']);
  assert.deepEqual(check('french', null, 'one', root), ['games/one/lang/french.json says it as the shared file does: "Only one"']);
  w('src/games/two/lang/french.json', { 'Only two': 'Seulement deux', 'Two again': 'Encore deux' });
  assert.deepEqual(check('french', null, 'two', root), ['not in use: "Only one"']);
  w('src/games/two/lang/french.json', '{ broken');
  assert.match(check('french', null, 'two', root).join('\n'), /^games\/two\/lang\/french\.json: /);
  w('src/games/two/lang/french.json', { 'Only two': 'Seulement deux', 'Two again': 'Encore deux' });
  // a language only two has: its own file has every text, and two lists it
  w('src/games/two/lang/finnish.json', { 'Only two': 'Vain kaksi', 'Shared markup': 'Jaettu merkintä' });
  assert.deepEqual(languages('one', root), ['french']);
  assert.deepEqual(languages('two', root), ['french', 'finnish']);
  assert.deepEqual(check('finnish', null, 'two', root), ['missing (games/two/lang/finnish.json): "Shared text"', 'missing (games/two/lang/finnish.json): "Two again"']);
  w('src/games/two/languages.ts', 'export const LANGUAGES = [\'english\', \'french\'];');
  assert.deepEqual(languageProblems('two', root), ['finnish: a file of the page, not in games/two/languages.ts']);
  w('src/games/two/languages.ts', 'export const LANGUAGES = [\'english\', \'french\', \'finnish\', \'danish\'];');
  assert.deepEqual(languageProblems('two', root), ['games/two/languages.ts lists danish, which has no file']);
  w('src/games/two/lang/klingon.json', {});
  assert.deepEqual(languageProblems('two', root), ['games/two/lang/klingon.json: not a language the page has (i18n.ts LOCALE)', 'games/two/languages.ts lists danish, which has no file']);
});

test('t(), tn() and tvars(): English, then a language\'s text and plural forms', async () => {
  const out = await esbuild.build({ entryPoints: [path.join(HERE, '../src/app/i18n.ts')], bundle: true, write: false, format: 'esm', platform: 'neutral' });
  const I = await import('data:text/javascript;base64,' + Buffer.from(out.outputFiles[0].text).toString('base64'));
  assert.equal(I.isLanguage('finnish'), false, 'Underground 2\'s page (tsconfig.json\'s @game) has no Finnish');
  assert.equal(I.t('Saved: {what}, {value}.', { what: 'Movies', value: 'off' }), 'Saved: Movies, off.');
  assert.equal(I.tn(1, '{n} lap', '{n} laps'), '1 lap');
  assert.equal(I.tn(5, '{n} lap', '{n} laps'), '5 laps');
  const pl = { '{n} laps': { one: '{n} okrążenie', few: '{n} okrążenia', many: '{n} okrążeń', other: '{n} okrążenia' }, 'the browser': 'przeglądarka',
    'Saved: {what}, {value}.': 'Zapisano: {what}, {value}.', '{Browser} asks': '{Browser} pyta' };
  globalThis.fetch = async url => ({ ok: url === '/lang/polish.json', status: 404, json: async () => pl });
  const log = [];
  assert.equal(await I.setLanguage('polish', s => log.push(s)), true);
  assert.equal(I.language(), 'polish');
  assert.equal(I.tn(1, '{n} lap', '{n} laps'), '1 okrążenie');
  assert.equal(I.tn(3, '{n} lap', '{n} laps'), '3 okrążenia');
  assert.equal(I.tn(5, '{n} lap', '{n} laps'), '5 okrążeń');
  assert.equal(I.t('Saved: {what}, {value}.', { what: 'Filmy', value: 'wyłączone' }), 'Zapisano: Filmy, wyłączone.');
  assert.equal(I.t('Not in the file'), 'Not in the file');
  assert.equal(I.tvars('{Browser} asks', { Browser: 'the browser' }), 'przeglądarka pyta');
  assert.equal(I.listOf(['a', 'b', 'c']), 'a, b i c');
  // a file that does not load leaves the page as it was
  assert.equal(await I.setLanguage('german', s => log.push(s)), false);
  assert.equal(I.language(), 'polish');
  assert.match(log.join('\n'), /german texts did not load \(Error: HTTP 404\)/);
  assert.equal(await I.setLanguage('english'), true);
  assert.equal(I.tn(5, '{n} lap', '{n} laps'), '5 laps');
  assert.equal(I.listOf(['a', 'b', 'c']), 'a, b and c');
});
