// Invite links (web/src/app/invite.ts): what a room's or a crew's link looks like, what the page reads from one, and
// which names it takes:
//   node --test web/test/invite.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * from './web/src/app/invite.ts'; export { roomHash } from './web/src/app/players.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const I = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
const O = 'https://nfsu2.xyz';

test('a room\'s link, and a crew\'s with the sender\'s room', () => {
  assert.equal(I.inviteLink(O, { room: 'Blue Room' }), 'https://nfsu2.xyz/#room=Blue%20Room');
  assert.equal(I.inviteLink(O, { crew: 'Night Owls', room: 'Blue Room' }), 'https://nfsu2.xyz/#crew=Night%20Owls&room=Blue%20Room');
  assert.equal(I.inviteLink(O, { crew: 'Night Owls', room: '' }), 'https://nfsu2.xyz/#crew=Night%20Owls&room=', 'in no room: says so');
  assert.equal(I.inviteLink(O, { crew: 'Night Owls' }), 'https://nfsu2.xyz/#crew=Night%20Owls');
  // letters past ASCII as they are (a chat shows them); the URL's own signs and what a chat cuts off a link's end coded
  assert.equal(I.inviteLink(O, { room: 'Комната' }), 'https://nfsu2.xyz/#room=Комната');
  assert.equal(I.inviteLink(O, { room: 'A&B=C#1?' }), 'https://nfsu2.xyz/#room=A%26B%3DC%231%3F');
  assert.equal(I.inviteLink(O, { room: 'Team (2).' }), 'https://nfsu2.xyz/#room=Team%20%282%29%2E');
  assert.equal(I.inviteLink(O, { room: '100%!' }), 'https://nfsu2.xyz/#room=100%25%21');
  assert.equal(I.inviteLink(O, { room: 'a b' }), 'https://nfsu2.xyz/#room=a%C2%A0b', 'a blank past ASCII: coded');
});

test('the page reads its own links back, the names as the settings keep them', () => {
  for (const inv of [{ room: 'Blue Room' }, { crew: 'Night Owls', room: 'Blue Room' }, { crew: 'Night Owls', room: '' }, { crew: 'Night Owls' },
    { room: 'Комната' }, { room: 'A&B=C#1?' }, { room: 'Team (2).' }, { room: '100%!' }, { crew: 'zoë', room: 'ŁÓDŹ ×2' }]) {
    const link = I.inviteLink(O, inv);
    assert.deepEqual(I.readInvite(new URL(link).hash), inv, link);
    assert.deepEqual(I.readInvite(link.slice(link.indexOf('#'))), inv, `${link}: as typed`);
  }
  assert.deepEqual(I.readInvite('#room=%20%20Blue%C2%A0%C2%A0Room%E2%80%8B%20'), { room: 'Blue Room' }, 'blanks tidied, invisible characters out');
  assert.deepEqual(I.readInvite('#room=Cafe%CC%81'), { room: 'Café' }, 'composed (NFC)');
  assert.deepEqual(I.readInvite('#room=Blue+Room'), { room: 'Blue Room' }, 'a + as a space, as a form would send it');
  assert.equal(I.readInvite(''), null);
  assert.equal(I.readInvite('#'), null);
  assert.equal(I.readInvite('#top'), null, 'another fragment');
  assert.equal(I.readInvite('#room='), null, 'no room');
  assert.equal(I.readInvite('#room=%E2%80%8B'), null, 'a room of an invisible character alone');
  assert.equal(I.readInvite('#room=%E0%A4'), null, 'not UTF-8');
  assert.deepEqual(I.readInvite('#x=1&room=Red&other'), { room: 'Red' }, 'other parts left alone');
});

test('which names it takes: the Room row\'s and the Crews box\'s checks', () => {
  assert.deepEqual(I.checkRoom('  Blue   Room '), { ok: true, name: 'Blue Room' });
  assert.deepEqual(I.checkRoom('Team#2'), { ok: true, name: 'Team#2' }, 'a # with no blank before it stays in the name');
  for (const cut of ['Team #2', 'Team ;2', '#2', ';x']) {
    const r = I.checkRoom(cut);
    assert.equal(r.ok, false, cut);
    assert.match(r.why, /no # or ; after a space/, cut);
  }
  assert.deepEqual(I.checkRoom('   '), { ok: false, why: null });
  assert.equal(I.checkRoom('x'.repeat(64)).ok, false, 'more than 63 bytes');
  assert.equal(I.checkRoom('я'.repeat(31)).ok, true, '62 bytes');
  assert.equal(I.checkRoom('я'.repeat(32)).ok, false, '64 bytes');
  assert.deepEqual(I.checkInvite({ room: 'Blue Room', crew: 'Night Owls' }), { ok: true });
  assert.deepEqual(I.checkInvite({ crew: 'Night Owls', room: '' }), { ok: true }, 'a crew in no room');
  const bad = I.checkInvite({ crew: 'a, b' });
  assert.equal(bad.ok, false);
  assert.equal(bad.v.name, 'a, b');
  assert.equal(I.checkInvite({ room: 'Team #2' }).ok, false);
  assert.equal(I.checkInvite({ crew: 'x'.repeat(25) }).ok, false, 'a crew of more than 24 characters');
});

test('one room however it is typed: the hash\'s rules', () => {
  assert.ok(I.sameName('Blue Room', 'blueroom'));
  assert.ok(I.sameName('КОМНАТА', 'комната'));
  assert.ok(I.sameName('Blue Room', 'BLUE ROOM'));
  assert.ok(!I.sameName('Blue', 'Red'));
  assert.ok(I.sameName('', '  '), 'no room is no room');
});
