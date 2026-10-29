// Claimed names in the page (web/src/app/claim.ts):
//   node --test web/test/claim.test.mjs
// - the Online form of a name: the same cases as nfsu2-claims' tests (web/server-rs/crates/claims) and runtime/online.c
//   clean(), which the game's Online menu uses
// - the digits: four in place of the end, the same ones for one browser and name, Player's where the cut reads as
//   a slur; the slur filter on the name as typed and on its Online form
// - /names: what is asked (the token as Bearer, never the keyphrase), the answers and refusals as the page reads them
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * as C from './web/src/app/claim.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const { C } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

test('the Online form, as the server and the game make it', () => {
  const cases = [
    ['Alex', 'Alex'], ['Alex the Great', 'Alex_the_Great'], ['  Alex!!', 'Alex'], ['a', 'a__'], ['é!?', null], ['', null],
    ['abcdefghijklmn opq', 'abcdefghijklmno'], ['abcdefghijklm nop', 'abcdefghijklm_n'], ['abcdefghijklmn op', 'abcdefghijklmno'],
    ['x\u{1F600}y', 'x_y'], ['Gökhan', 'G_khan'],
  ];
  for (const [i, o] of cases) assert.equal(C.onlineName(i), o, JSON.stringify(i));
});

test('the slur filter on the name as typed and on its Online form', () => {
  for (const n of ['Nigger', 'N1gg3r', 'n i g g e r', 'Hitler', 'Nіgger']) assert.ok(C.readsAsSlur(n), n);
  for (const n of ['Alex', 'Kurwa', 'RapidRacer', 'NAME']) assert.ok(!C.readsAsSlur(n), n);
});

test('the digits: the same for one browser and name, the end of the name replaced', async () => {
  const a = await C.digitsFor('NAME', 'seed-1');
  assert.match(a, /^NAME\d{4}$/);
  assert.equal((await C.digitsFor('name', 'seed-1')).slice(4), a.slice(4), 'the same digits in any case');
  assert.equal(await C.digitsFor('NAME', 'seed-1'), a);
  const others = new Set(await Promise.all(Array.from({ length: 20 }, (_, i) => C.digitsFor('NAME', `seed-${i}`))));
  assert.ok(others.size > 15, 'other browsers, other digits');
  assert.match(await C.digitsFor('Alex the Great', 's'), /^Alex_the_Gr\d{4}$/);
  // a cut that reads as a slur: Player's digits
  const d = await C.digitsFor('AbcdefghFagxyz', 's');
  assert.match(d, /^(Player\d{4}|AbcdefghFag\d{4})$/);
  assert.ok(!C.readsAsSlur(d), d);
});

test('/names: asked with the token, read as the page reads it', async () => {
  const calls = [];
  let answer = { status: 200, body: { name: 'Alex', state: 'free', captcha: 'site' } };
  const f = async (url, init = {}) => {
    calls.push({ url, init });
    return { status: answer.status, ok: answer.status < 300, json: async () => answer.body };
  };
  let token = null;
  const c = new C.NameClaims(async () => token, f);
  assert.deepEqual(await c.check('Alex', 'Alex4821'), { name: 'Alex', state: 'free', captcha: 'site' });
  assert.equal(calls[0].url, '/names?name=Alex&alt=Alex4821');
  assert.equal(calls[0].init.headers.authorization, undefined, 'no keyphrase: no token');
  assert.equal(c.here, true);
  token = 'A'.repeat(43);
  await c.check('Alex');
  assert.equal(calls[1].init.headers.authorization, `Bearer ${'A'.repeat(43)}`);
  answer = { status: 404, body: {} };
  assert.equal(await c.check('Alex'), null);
  assert.equal(c.here, false, 'a server without claims');
  // a claim: the name and Cloudflare's answer; refusals as the server says them
  answer = { status: 200, body: { state: 'yours', mine: { name: 'Alex', until: 1, next: 0 } } };
  assert.deepEqual(await c.claim('Alex', 'cf-answer'), { ok: true, mine: { name: 'Alex', until: 1, next: 0 } });
  const post = calls.at(-1);
  assert.equal(post.init.method, 'POST');
  assert.deepEqual(JSON.parse(post.init.body), { name: 'Alex', captcha: 'cf-answer' });
  answer = { status: 429, body: { error: 'soon', next: 99 } };
  assert.deepEqual(await c.claim('Racer', null), { ok: false, error: 'soon', next: 99 });
  answer = { status: 429, body: {} };
  assert.deepEqual(await c.claim('Racer', null), { ok: false, error: 'busy' });
  answer = { status: 409, body: { error: 'taken' } };
  assert.deepEqual(await c.claim('Racer', null), { ok: false, error: 'taken' });
  const broken = new C.NameClaims(async () => token, async () => { throw new Error('offline'); });
  assert.equal(await broken.check('Alex'), null);
  assert.deepEqual(await broken.claim('Alex', null), { ok: false, error: 'net' });
});
