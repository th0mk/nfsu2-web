// The Crews setting (settings.ini roam_crew; web/src/app/crews.ts, settings.ts, settings-ui.ts, players.ts):
//   node --test web/test/crews.test.mjs
// - the crews a value names, as the game takes them: split at commas, trimmed, the empty ones and repeats (by hash:
//   capitals and spaces do not count) left out, the first 4
// - a list written back through settings.ini to NFSU2_ROAM_CREW, a "; comment" cut off, a crew longer than 24
//   characters refusing the line, an older file getting the option's paragraph
// - what the panel's box lets join: nothing typed, a comma, # or ;, too long, a slur, a repeat by hash, a fifth
// - the Room setting refuses a name that reads as a slur, as Your name does
// - Reset all settings keeps the crews, as it keeps the name
// - /players?crews=: the hashes in the player's order (beside room=), the counts by hash, none from an older server,
//   and a late answer to an earlier question not shown over a later one
// The modules are bundled from web/src with esbuild, as web/test/settings-reset.test.mjs does.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * from './web/src/app/settings.ts'; export * as C from './web/src/app/crews.ts';
export * as P from './web/src/app/players.ts'; export * as UI from './web/src/app/settings-ui.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const S = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));
const { C, P } = S;

test('the crews a value names', () => {
  assert.deepEqual(C.crewList(''), []);
  assert.deepEqual(C.crewList('Blue Room, Night Owls'), ['Blue Room', 'Night Owls']);
  assert.deepEqual(C.crewList(' Night Owls ,Blue Room,,  , '), ['Night Owls', 'Blue Room'], 'the player\'s order, trimmed, empty ones out');
  assert.deepEqual(C.crewList('Blue Room, blueroom, BLUE ROOM, Red'), ['Blue Room', 'Red'], 'the same hash: the first spelling');
  assert.deepEqual(C.crewList('a, b, c, d, e, f'), ['a', 'b', 'c', 'd'], 'the first 4');
  assert.deepEqual(C.crewList('a, A, b, c, d, e'), ['a', 'b', 'c', 'd'], 'a repeat does not take a place');
  assert.deepEqual(C.crewList('Blue\t \tRoom'), ['Blue Room'], 'blanks inside as one space (the hash skips them)');
  assert.deepEqual(C.crewList('Zoë, zoË, КОМАНДА, команда'), ['Zoë', 'КОМАНДА'], 'capitals past A to Z small too (players.ts roomFold)');
  assert.equal(C.crewsValue(['Blue Room', 'Night Owls']), 'Blue Room, Night Owls');
  assert.deepEqual(C.crewList(C.crewsValue(['Blue Room', 'Night Owls', 'x'])), ['Blue Room', 'Night Owls', 'x']);
  assert.deepEqual(C.CREW_PALETTE, ['3cd2ff', 'ff50c8', 'a078ff', 'ff4646', '4f7dff', 'fff03c', '3cf0f0', 'ff9ad5'], 'Thom\'s 8, the defaults first');
  assert.equal(C.CREW_COLOUR_NAMES.length, C.CREW_PALETTE.length, 'a name for each');
});

test('the crews\' colours: the value bundle.m\'s K_RGBS takes', () => {
  // at most 4 entries, each blanks (spaces, tabs), none or 6 hex digits of any case, blanks
  for (const v of ['3cd2ff', '3cd2ff, ff4646', '3CD2FF,ff4646', ' 3cd2ff\t,\tff4646 ', ',', ', ,', ', ff4646', 'a078ff, , , ff4646', '000000, FFFFFF', '   '])
    assert.equal(C.rgbsOk(v), true, JSON.stringify(v));
  for (const v of ['3cd2ff, ff4646, a078ff, ff4646, 4f7dff', ',,,,', '3cd', '#3cd2ff', '3cd2ff0', 'xyz', '3cd2 ff', '3cd2ff\v', '\v3cd2ff', '3cd2ff\r',
    'ggggggg', '3cd2ff;', '3cd2fg', '３cd2ff'])
    assert.equal(C.rgbsOk(v), false, JSON.stringify(v));
  // the setting: a "; comment" cut off first, as for the other free-roam keys
  assert.equal(S.keyDef('roam_crew_colours').env, 'NFSU2_ROAM_CREW_RGB');
  assert.equal(S.DEFAULTS.get('roam_crew_colours'), '');
  assert.deepEqual(S.checkSetting('roam_crew_colours', '3cd2ff, ff4646 ; mine'), { ok: true, env: 'NFSU2_ROAM_CREW_RGB', value: '3cd2ff, ff4646' });
  assert.deepEqual(S.checkSetting('roam_crew_colours', '3cd2ff #1'), { ok: true, env: 'NFSU2_ROAM_CREW_RGB', value: '3cd2ff' }, 'a # after a blank is a comment');
  const bad = S.checkSetting('roam_crew_colours', '3cd2ff, #ff4646');
  assert.equal(bad.ok, true, 'there too: "3cd2ff," is what is left');
  assert.equal(S.checkSetting('roam_crew_colours', '3cd2ff,#ff4646').ok, false, 'a # inside an entry');
  assert.match(S.checkSetting('roam_crew_colours', 'red').reason, /is not a list of up to 4 colours of 6 hex digits \(rrggbb\), separated by commas/);
  // the crews with their colours: entry i, else the place's default; a refused value counts as none
  assert.deepEqual(C.crewsOf('Blue Room, Night Owls, Drift', 'FF4646'), [{ name: 'Blue Room', rgb: 'ff4646' }, { name: 'Night Owls', rgb: 'ff50c8' }, { name: 'Drift', rgb: 'a078ff' }]);
  assert.deepEqual(C.crewsOf('a, b', ', 4f7dff, 3cf0f0'), [{ name: 'a', rgb: '3cd2ff' }, { name: 'b', rgb: '4f7dff' }], 'an empty entry: the default; one past the crews: unused');
  assert.deepEqual(C.crewsOf('a, A, b', '111111, 222222'), [{ name: 'a', rgb: '111111' }, { name: 'b', rgb: '222222' }], 'by the crews the game takes (a repeat has no entry)');
  assert.deepEqual(C.crewsOf('a', 'bogus').map(c => c.rgb), ['3cd2ff']);
  assert.deepEqual(C.crewsOf('', '3cd2ff'), []);
  assert.deepEqual(C.rgbWords(C.crewsOf('a, b', '3cd2ff, ff4646')), [0x3cd2ff, 0xff4646]);
});

test('the crews\' colours stay with their crews: Leave, Join, a colour picked', () => {
  // as the panel and the menu change them (settings-ui.ts crewControl): a Leave keeps the others' colours, a Join takes
  // the first colour no crew has, and the line is written with every crew's colour
  let list = C.crewsOf('Blue Room, Night Owls, Drift', '');
  assert.deepEqual(list.map(c => c.rgb), ['3cd2ff', 'ff50c8', 'a078ff'], 'the defaults by place');
  list = list.filter(c => c.name !== 'Blue Room');
  assert.equal(C.rgbsValue(list), 'ff50c8, a078ff', 'a Leave: the others keep theirs, written');
  const back = C.crewsOf(C.crewsValue(list.map(c => c.name)), C.rgbsValue(list));
  assert.deepEqual(back, list, 'and read back the same');
  list = [...list, { name: 'Red', rgb: C.freeColour(list) }];
  assert.equal(list[2].rgb, '3cd2ff', 'a Join: the first colour no crew has (the defaults first)');
  list = [...list, { name: 'Four', rgb: C.freeColour(list) }];
  assert.equal(list[3].rgb, 'ff4646');
  assert.equal(C.freeColour([{ name: 'a', rgb: '3cd2ff' }, { name: 'b', rgb: 'ff50c8' }, { name: 'c', rgb: 'a078ff' }, { name: 'd', rgb: 'ff4646' }]), '4f7dff', 'past the defaults');
  // a colour picked; two crews may share one
  list = list.map((c, i) => i === 0 ? { ...c, rgb: 'a078ff' } : c);
  assert.equal(C.rgbsValue(list), 'a078ff, a078ff, 3cd2ff, ff4646');
  const v = S.checkSetting('roam_crew_colours', C.rgbsValue(list));
  assert.equal(v.ok, true);
  assert.deepEqual(C.crewsOf(C.crewsValue(list.map(c => c.name)), v.value), list);
  // the defaults at their places are written too (a later Leave moves the crews up a place)
  assert.equal(C.rgbsValue(C.crewsOf('a, b', '')), '3cd2ff, ff50c8');
});

test('settings.ini: roam_crew to NFSU2_ROAM_CREW', () => {
  assert.equal(S.keyDef('roam_crew').env, 'NFSU2_ROAM_CREW');
  assert.equal(S.DEFAULTS.get('roam_crew'), '', 'in the template, empty');
  const ini = S.settingsPut(S.TEMPLATE, 'roam_crew', 'Blue Room, Night Owls');
  assert.match(ini, /^roam_crew = Blue Room, Night Owls$/m);
  assert.doesNotMatch(ini, /^#roam_crew =/m, 'in the template line\'s place');
  assert.equal(S.resolveEnv(ini, {}, {}).env.NFSU2_ROAM_CREW, 'Blue Room, Night Owls');
  // a free-roam key: "; comment" after a blank is cut off
  assert.deepEqual(S.checkSetting('roam_crew', 'Blue Room, Night Owls ; ours'), { ok: true, env: 'NFSU2_ROAM_CREW', value: 'Blue Room, Night Owls' });
  // 24 characters each (characters, not bytes: 24 ü are 48)
  assert.equal(S.checkSetting('roam_crew', `${'x'.repeat(24)}, ${'ü'.repeat(24)}`).ok, true);
  const long = S.checkSetting('roam_crew', `Blue, ${'y'.repeat(25)}`);
  assert.equal(long.ok, false);
  assert.match(long.reason, /is not a list of crew names \(at most 24 characters each, separated by commas\)/);
  assert.equal(S.checkSetting('roam_crew', 'a\x01b').ok, false, 'no control characters');
  assert.equal(S.checkSetting('roam_crew', 'a, b, c, d, e, a').ok, true, 'more than 4, and repeats: the game takes the first 4');
  const r = S.resolveEnv(`roam_crew = Blue, ${'y'.repeat(25)}\n`, {}, {});
  assert.equal(r.env.NFSU2_ROAM_CREW, undefined, 'a refused line sets nothing');
  assert.ok(r.file.roam_crew.refused);
  // a file of an older version gets the option's paragraph (only that one), commented out
  const para = /^# Your crews,[^]*?^#roam_crew =\n/m.exec(S.TEMPLATE)[0];
  const old = S.TEMPLATE.replace(para + '\n', '');
  assert.notEqual(old, S.TEMPLATE);
  const up = S.settingsUpgrade(old);
  assert.deepEqual(up.added, ['roam_crew']);
  assert.ok(up.text.endsWith('did not list yet) ----\n\n' + para), up.text.slice(-800));
  assert.equal(up.text.split('\n').filter(l => l.startsWith('#roam_room')).length, 1, 'not the other free-roam options again');
  // and without the colours' paragraph either: both, in the template's order
  const para2 = /^# Each crew's colour,[^]*?^#roam_crew_colours =\n/m.exec(S.TEMPLATE)[0];
  const up2 = S.settingsUpgrade(S.TEMPLATE.replace(para + '\n', '').replace(para2 + '\n', ''));
  assert.deepEqual(up2.added, ['roam_crew', 'roam_crew_colours']);
  assert.ok(up2.text.endsWith('did not list yet) ----\n\n' + para + '\n' + para2), up2.text.slice(-800));
});

test('what the box lets join', () => {
  const list = ['Blue Room', 'Night Owls'];
  assert.deepEqual(C.checkCrew('', list), { ok: false, why: null }, 'nothing typed: nothing to say');
  assert.deepEqual(C.checkCrew('  \t ', list), { ok: false, why: null });
  assert.deepEqual(C.checkCrew('  Red  Line ', list), { ok: true, name: 'Red Line' }, 'trimmed');
  assert.equal(C.checkCrew('Red, Line', list).why, 'No commas, # or ; in a crew name.');
  assert.equal(C.checkCrew('#1', list).why, 'No commas, # or ; in a crew name.');
  assert.equal(C.checkCrew('a;b', list).why, 'No commas, # or ; in a crew name.');
  assert.equal(C.checkCrew('y'.repeat(25), list).why, 'At most 24 characters.');
  assert.equal(C.checkCrew('y'.repeat(24), list).ok, true);
  assert.equal(C.checkCrew('ü'.repeat(24), list).ok, true, 'characters, not bytes');
  assert.deepEqual(C.checkCrew('blueroom', list), { ok: false, why: 'Already in your crews: {name}.', name: 'Blue Room' }, 'a repeat by hash');
  assert.deepEqual(C.checkCrew('NIGHT OWLS', list), { ok: false, why: 'Already in your crews: {name}.', name: 'Night Owls' });
  assert.deepEqual(C.checkCrew('Zoë', ['zoË']), { ok: false, why: 'Already in your crews: {name}.', name: 'zoË' }, 'capitals past A to Z small too');
  assert.equal(C.checkCrew('Fifth', ['a', 'b', 'c', 'd']).why, 'Up to 4 crews: leave one to join another.');
  assert.equal(C.checkCrew('', ['a', 'b', 'c', 'd']).why, 'Up to 4 crews: leave one to join another.', 'said with nothing typed too');
  // a name that reads as a slur (names.mjs, as a player's name) is no crew; the words round them still are
  for (const typed of ['N1gg 4', 'f4g_42', 'XxCoonXx', 'Paki Crew', 'ｆａｇ'])
    assert.equal(C.checkCrew(typed, list).why, 'Not a crew name: it reads as a slur.', typed);
  for (const typed of ['Mustafa GS', 'Raccoon', 'Van Dyke', 'Spice Girls']) assert.equal(C.checkCrew(typed, list).ok, true, typed);
  assert.deepEqual(C.crewList('Blue Room, XxCoonXx, Night Owls'), ['Blue Room', 'Night Owls'], 'a hand-written one left out, as the game does');
  assert.deepEqual(C.crewsOf('a, f4g_42, b', '111111, 222222, 333333').map(c => c.name), ['a', 'b'], 'and it takes no place');
  // whatever joins makes a value the game reads back the same
  let crews = [];
  for (const typed of [' Blue Room', 'night owls', 'Ünïcode ✓', 'x'.repeat(24)]) {
    const c = C.checkCrew(typed, crews);
    assert.equal(c.ok, true, typed);
    crews = [...crews, c.name];
    const v = S.checkSetting('roam_crew', C.crewsValue(crews));
    assert.equal(v.ok, true);
    assert.deepEqual(C.crewList(v.value), crews);
  }
});

test('the Room setting: a name that reads as a slur is refused, as Your name is', () => {
  assert.equal(S.keyDef('roam_room').kind, 'room');
  assert.deepEqual(S.checkSetting('roam_room', 'Team ; ours'), { ok: true, env: 'NFSU2_ROAM_ROOM', value: 'Team' });
  assert.equal(S.checkSetting('roam_room', 'Mustafa GS').ok, true);
  for (const v of ['XxCoonXx', 'f4g_42 ; mine', 'ｆａｇ']) {
    const r = S.checkSetting('roam_room', v);
    assert.equal(r.ok, false, v);
    assert.equal(r.why, '"{value}" is not a room name (it reads as a slur)', v);
  }
  assert.match(S.checkSetting('roam_room', 'z'.repeat(64)).reason, /is not text of 1 to 63 bytes/);
  assert.equal(S.resolveEnv('roam = on\nroam_room = XxCoonXx\n', {}, {}).env.NFSU2_ROAM_ROOM, undefined, 'no room at Play');
});

test('Reset all settings keeps the crews and their colours', () => {
  let ini = S.TEMPLATE;
  for (const [k, v] of [['roam_name', 'Lessis'], ['roam_crew', 'Blue Room, Night Owls'], ['roam_crew_colours', 'ff4646, 3cd2ff'], ['fps', '60'], ['roam_room', 'x']]) ini = S.settingsPut(ini, k, v);
  const out = S.UI.resetAll(ini);
  const r = S.resolveEnv(out, {}, {});
  assert.deepEqual(Object.keys(r.byEnv).sort(), ['NFSU2_ROAM_CREW', 'NFSU2_ROAM_CREW_RGB', 'NFSU2_ROAM_NAME']);
  assert.equal(r.env.NFSU2_ROAM_CREW, 'Blue Room, Night Owls');
  assert.equal(r.env.NFSU2_ROAM_CREW_RGB, 'ff4646, 3cd2ff');
  assert.match(out, /^roam_crew = Blue Room, Night Owls\n[^]*^roam_crew_colours = ff4646, 3cd2ff$/m);
  assert.equal(S.UI.resetAll(out), out, 'nothing more to reset');
  // a refused line is not kept
  assert.equal(S.UI.resetAll(`roam_crew = ${'y'.repeat(25)}\nroam_crew_colours = #ff4646\n`), S.TEMPLATE);
  assert.ok(S.UI.PANEL_KEYS.includes('roam_crew_colours'), 'the Crews row writes it');
});

test('/players?crews=: asked in the player\'s order, the counts by hash', async () => {
  const blue = P.roomHash('Blue Room'), owls = P.roomHash('Night Owls'), room = P.roomHash('Team');
  assert.equal(blue, 0x160fba32, 'the Room setting\'s hash');
  assert.deepEqual([...P.crewCounts({ players: 3, online: 0, crews: [2, 0] }, [blue, owls])], [[blue, 2], [owls, 0]]);
  assert.deepEqual([...P.crewCounts({ players: 3, online: 0 }, [blue, owls])], [], 'an older server: not known');
  assert.deepEqual([...P.crewCounts({ players: 3, online: 0, crews: [1, -1, 'x'] }, [blue, owls, room])], [[blue, 1]], 'only counts');
  // watchPlayers against a stand-in server
  const asked = [], fetch0 = globalThis.fetch;
  let answer = { players: 3, online: 1, roam: 2, room: 1, crews: [2, 0] };
  globalThis.window = { setInterval: () => 0 };
  globalThis.document = { visibilityState: 'visible', addEventListener() {} };
  globalThis.fetch = async url => { asked.push(url); return { status: 200, ok: true, json: async () => answer }; };
  let crews = [blue, owls], name = 'Team';
  const shown = [];
  let next;
  const w = P.watchPlayers((c, inRoom, counts) => { shown.push({ line: c ? P.roomLine(c, inRoom) : '', counts }); next?.(); }, () => true, undefined, () => name, () => crews);
  const wait = () => new Promise(r => { next = r; });
  await wait();
  assert.equal(asked[0], `/players?room=${room}&crews=${blue},${owls}`);
  assert.deepEqual([...shown[0].counts], [[blue, 2], [owls, 0]]);
  assert.equal(shown[0].line, 'You are in room "Team": 1 in free roam there.', 'the Room row\'s line');
  crews = [owls]; name = ''; answer = { players: 3, online: 1, roam: 2, crews: [5] };
  w.now(); await wait();
  assert.equal(asked[1], `/players?crews=${owls}`);
  assert.deepEqual([...shown[1].counts], [[owls, 5]]);
  crews = []; answer = { players: 3, online: 1, roam: 2 };
  w.now(); await wait();
  assert.equal(asked[2], '/players', 'no crews, no room: as before');
  assert.equal(shown[2].counts.size, 0);
  delete globalThis.window; delete globalThis.document; globalThis.fetch = fetch0;
});

test('/players: an answer that comes after a later question\'s is not shown (the page\'s first, before settings.ini)', async () => {
  const blue = P.roomHash('Blue Room'), fetch0 = globalThis.fetch;
  globalThis.window = { setInterval: () => 0 };
  globalThis.document = { visibilityState: 'visible', addEventListener() {} };
  // the first question (no crews yet) waits until the second (with them) is answered
  const held = [];
  globalThis.fetch = async url => {
    const answer = url.includes('crews=') ? { players: 3, online: 0, roam: 2, crews: [2] } : { players: 3, online: 0, roam: 2 };
    const r = { status: 200, ok: true, json: async () => answer };
    return url.includes('crews=') ? r : new Promise(res => held.push(() => res(r)));
  };
  let crews = [];
  const shown = [];
  const w = P.watchPlayers((c, inRoom, counts) => shown.push(counts), () => true, undefined, () => '', () => crews);
  await new Promise(r => setTimeout(r, 0));
  crews = [blue];
  const fresh = await w.fresh(1000);
  assert.deepEqual(shown.map(c => [...c]), [[[blue, 2]]], 'the second answer, with the crew');
  held.forEach(f => f()); await new Promise(r => setTimeout(r, 0));
  assert.deepEqual(shown.map(c => [...c]), [[[blue, 2]]], 'the first, late, left out');
  assert.deepEqual(w.last(), fresh, 'and the newest answer is still the second');
  assert.deepEqual(w.last().crews, [2]);
  delete globalThis.window; delete globalThis.document; globalThis.fetch = fetch0;
});
