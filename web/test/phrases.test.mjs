// Quick chat's phrases and the player's sixteen (web/src/app/phrases.ts; settings.ts quick_chat_phrases,
// quick_chat_groups):
//   node --test web/test/phrases.test.mjs
// - the numbers are the protocol: the phrases there were keep their numbers (a digest of the first 152 in order), a
//   new one only goes at the end, and no text is there twice
// - the two lines' values: sixteen numbers the library has, four of the categories; anything else refused at Play
//   (the default instead), and the defaults the template shows are the first sixteen in Hi, Reply, Drive, Race, but
//   the countdown (152, the special phrase) in One more?'s place
// The modules are bundled from web/src with esbuild, as web/test/settings-reset.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * from './web/src/app/settings.ts'; export * as Q from './web/src/app/phrases.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const S = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
const { Q } = S;

test('the phrases keep their numbers', () => {
  const texts = Q.PHRASES.map(p => p.text);
  assert.deepEqual(texts.slice(0, 16), ['Hey!', 'Nice car!', 'Thanks!', 'Bye!', 'Yes', 'No', 'Sorry!', 'lol',
    'Follow me!', 'Wait up!', 'Let\'s go!', 'Nice!', 'Race?', 'Join my race!', 'gg', 'One more?'], 'the first sixteen, as before');
  assert.ok(texts.length >= 152 && texts.length <= 256, `a number is a byte on the wire (${texts.length})`);
  const digest = crypto.createHash('sha256').update(texts.slice(0, 152).join('\n')).digest('hex').slice(0, 16);
  assert.equal(digest, '84085c069dc067dc', 'phrases 0-151 moved or changed: add new ones at the end instead');
  assert.equal(texts[99], 'Outrun?'); assert.equal(texts[151], 'On my phone!');
  assert.equal(new Set(texts).size, texts.length, 'no phrase twice');
  const cats = new Set(Q.CATS.map(c => c.id));
  for (const p of Q.PHRASES) assert.ok(cats.has(p.cat), `${p.text}: a category the groups can take`);
  for (const c of Q.CATS) assert.ok(Q.PHRASES.some(p => p.cat === c.id), `${c.id}: not empty`);
  assert.equal(Q.COUNTDOWN, 152); assert.equal(texts[152], 'Countdown');
  assert.equal(Q.PHRASES[152].special, 'countdown', 'the countdown is the special one (roam_game.c SAY_COUNTDOWN 152)');
  assert.deepEqual(Q.PHRASES.filter(p => p.special).map(p => p.text), ['Countdown'], 'no other special phrase');
});

test('the player\'s sixteen', () => {
  const all = Array.from({ length: 16 }, (_, i) => i), dflt = [...all.slice(0, 15), 152];
  assert.deepEqual(Q.DEFAULT_PHRASES, dflt, 'the first fifteen and the countdown');
  assert.equal(Q.phrasesOk(all.join(',')), true);
  assert.equal(Q.phrasesOk('151, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14'), true, 'blanks after commas');
  assert.equal(Q.phrasesOk('0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0'), true, 'a phrase in more than one slot');
  assert.equal(Q.phrasesOk(all.slice(1).join(',')), false, 'fifteen');
  assert.equal(Q.phrasesOk([...all, 1].join(',')), false, 'seventeen');
  assert.equal(Q.phrasesOk([Q.PHRASES.length, ...all.slice(1)].join(',')), false, 'a number the library does not have');
  assert.equal(Q.phrasesOk(['-1', ...all.slice(1)].join(',')), false);
  assert.equal(Q.phrasesOk(['x', ...all.slice(1)].join(',')), false);
  assert.deepEqual(Q.phrasesOf(undefined), dflt);
  assert.deepEqual(Q.phrasesOf('nonsense'), dflt, 'refused: the default');
  assert.deepEqual(Q.phrasesOf(all.join(',')), all, 'the old default, saved: kept');
  assert.deepEqual(Q.phrasesOf([99, ...all.slice(1)].join(',')), [99, ...all.slice(1)]);
  assert.equal(Q.groupsOk('hi,reply,drive,race'), true);
  assert.equal(Q.groupsOk('Meet, React, help, nice'), true, 'any case, blanks');
  assert.equal(Q.groupsOk('hi,hi,hi,hi'), true);
  assert.equal(Q.groupsOk('hi,reply,drive'), false);
  assert.equal(Q.groupsOk('hi,reply,drive,party'), false);
  assert.deepEqual(Q.groupsOf('Meet, React, help, nice'), ['meet', 'react', 'help', 'nice']);
  assert.deepEqual(Q.groupsOf(''), ['hi', 'reply', 'drive', 'race']);
});

test('settings.ini: the two lines', () => {
  assert.equal(S.DEFAULTS.get('quick_chat_phrases'), Q.DEFAULT_PHRASES.join(','));
  assert.equal(S.DEFAULTS.get('quick_chat_groups'), Q.DEFAULT_GROUPS.join(','));
  const ok = S.resolveEnv('quick_chat_phrases = 99,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 ; mine\nquick_chat_groups = race,reply,drive,meet\n', {}, {});
  assert.equal(ok.env.NFSU2_ROAM_SAY_PHRASES, '99,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15', 'a "; comment" cut off');
  assert.equal(ok.env.NFSU2_ROAM_SAY_GROUPS, 'race,reply,drive,meet');
  const bad = S.resolveEnv('quick_chat_phrases = 1,2,3\nquick_chat_groups = hi,hi\n', {}, {});
  assert.equal(bad.env.NFSU2_ROAM_SAY_PHRASES, undefined, 'refused');
  assert.equal(bad.env.NFSU2_ROAM_SAY_GROUPS, undefined, 'refused');
  assert.ok(bad.file.quick_chat_phrases.refused && bad.file.quick_chat_groups.refused);
  assert.deepEqual(Q.phrasesOf(bad.env.NFSU2_ROAM_SAY_PHRASES), Q.DEFAULT_PHRASES, 'the page plays the default');
});
