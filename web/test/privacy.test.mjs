// The log as it leaves the browser (web/src/app/privacy.ts): names, addresses and the time zone's name replaced,
// everything else as it was.  Every name here is made up.
//   node --test web/test/privacy.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const bundle = async f => import('data:text/javascript;base64,' + Buffer.from((await esbuild.build({ entryPoints: [path.join(ROOT, f)], bundle: true, format: 'esm', write: false })).outputFiles[0].text).toString('base64'));
const { Privacy } = await bundle('web/src/app/privacy.ts');
const { LogRing } = await bundle('web/src/app/logs.ts');
const { LastGameKeeper } = await bundle('web/src/app/lastgame.ts');

const LOG = `[app] settings.ini:12: roam_name -> NFSU2_ROAM_NAME=Moonlit Rider
[app] settings.ini:14: roam_crew -> NFSU2_ROAM_CREW=Night Shift, Apex
[app] settings.ini:15: roam_room -> NFSU2_ROAM_ROOM=hideout
[0!] tz: Europe/Nowhere, UTC+1 h, daylight saving (2 transitions)
[0!] [hle] GetTimeZoneInformation: bias -60 min, "Nowhere Standard Time" / "Nowhere Summer Time", daylight 0310 2:00, standard 1027 3:00 (local) -> daylight
[app] save folder: OPFS save (/save); canvas 1280x800 device pixels, display 60 Hz; Mozilla/5.0 (Macintosh) Chrome/155.0.0.0 Safari/537.36
[net] on the virtual LAN as 10.77.5.98
[3!] [net] mode vlan: hub switch, address 10.77.5.98, host name VLAN-1378, adapter 02:4e:56:4c:05:62
[4!] [roam] free-roam multiplayer ON: id 32d05e60, port 47210, 0 configured peer(s), room "hideout" (1a2b3c4d), up to 8 players, 5 remote cars, 20 Hz, ghost cars
[4!] [roam] crew 1: "Night Shift" (1234abcd), colour ff0000
[4!] [roam] JOIN: Zeta Q joined (peer 3)
[4!] [roam] f=120 slot 2: loading Zeta Q's CIVIC (12 parts, skin 3, handle 0x1a2b)
[4!] [roam] f=130 slot 2: Zeta Q's CIVIC ready
[notice] Zeta Q joined free roam
[4!] [roam] JOIN: driver joined (peer 4)
[4!] [gpu] encode failed: update the graphics driver
[4!] [roam] f=200 hit with driver's car: 3.1 m/s
[4!] [roam] JOIN: 500 joined (peer 5)
[4!] [gpu] 2 frame(s) waited over 500 ms for the GPU
[4!] [carsfx] 500: nitrous on, 80% (voice 2)
[4!] [horn] Zeta Q's horn, 40 m away (single)
[chat] Zeta Q said 3 (gg)
[chat] you said 1 (lol)
[4!] [net] virtual LAN: tcp connect 10.77.5.98:3658 -> 10.77.9.119:3659
[4!] [mprace] f=900 join the host's LAN server at 10.77.9.119 (its advert not seen)
[4!] [mprace] f=950 the LAN name Moonlit Rider
[4!] [online] Online menu as Moonlit Rider (roam_name): no EA Login screens
[app] the game saved profile CARLA7 (1 this game)
[0!] [write] CreateFileA(OPEN_ALWAYS, 0x1) "C:\\NFSU2\\NFS Underground 2/CARLA7/CARLA7" -> "/save/NFS Underground 2/CARLA7/CARLA7"
[2!] [lan] the self-hosted server at 192.168.1.20 answered
[18!] [guest] 205 auth NAME 'Rook Nine' UID '' MID '$024e564c0562' MAC '$024e564c0562' VERS 'pc/1.2-Feb  9 2005' SKU '14705'
[1!] [thread] end tid=500 (returned, eax=0x0)
[app] settings.ini:3: roam -> NFSU2_ROAM=1
[4!] [roam] JOIN: 3 joined (peer 9)
  --- native state applier (12 types 7/3/1, 0 left)
`;

test('names, addresses and the zone go; the rest stays as it was', () => {
  const out = new Privacy('salt').scrub(LOG);
  for (const gone of ['Moonlit Rider', 'Night Shift', 'Apex', 'hideout', 'Zeta Q', 'CARLA7', 'Rook Nine', 'Europe/Nowhere', 'Nowhere Standard Time',
    '10.77.5.98', '10.77.9.119', '192.168.1.20', 'VLAN-1378', '05:62', "driver's", ' 500:', 'JOIN: 500'])
    assert.ok(!out.includes(gone), `${gone} is still in:\n${out}`);
  const lines = out.split('\n');
  assert.equal(lines[0], '[app] settings.ini:12: roam_name -> NFSU2_ROAM_NAME=you');
  assert.match(lines[1], /^\[app\] settings\.ini:14: roam_crew -> NFSU2_ROAM_CREW=crew-\w{4}, crew-\w{4}$/);
  assert.match(lines[3], /^\[0!\] tz: \(zone\), UTC\+1 h, daylight saving \(2 transitions\)$/);
  assert.match(lines[4], /GetTimeZoneInformation: bias -60 min, "\(zone\)" \/ "\(zone\)", daylight 0310/);
  assert.ok(lines[5].includes('Chrome/155.0.0.0 Safari/537.36'), 'a version is no address');
  assert.equal(lines[6], '[net] on the virtual LAN as vlan-you');
  assert.match(lines[7], /address vlan-you, host name VLAN-n, adapter 02:4e:56:4c:xx:xx$/);
  // a friend keeps one stand-in through the file, the log's own words stay
  const zeta = /JOIN: (player-\w{4}) joined/.exec(lines[10])[1];
  assert.ok(lines[11].includes(`loading ${zeta}'s CIVIC`) && lines[13] === `[notice] ${zeta} joined free roam`);
  assert.equal(lines[15], '[4!] [gpu] encode failed: update the graphics driver');
  assert.match(lines[16], /hit with player-\w{4}'s car: 3\.1 m\/s$/);
  assert.equal(lines[18], '[4!] [gpu] 2 frame(s) waited over 500 ms for the GPU');
  assert.match(lines[19], /^\[4!\] \[carsfx\] player-\w{4}: nitrous on, 80% \(voice 2\)$/);
  assert.equal(lines[22], '[chat] you said 1 (lol)');
  assert.match(lines[23], /tcp connect vlan-you:3658 -> (vlan-\w{4}):3659$/);
  assert.ok(lines[24].includes(`server at ${/-> (vlan-\w{4})/.exec(lines[23])[1]} (its`), 'the same address, the same stand-in');
  assert.equal(lines[25], '[4!] [mprace] f=950 the LAN name you');
  assert.match(lines[27], /^\[app\] the game saved profile profile-\w{4} \(1 this game\)$/);
  assert.match(lines[28], /"\/save\/NFS Underground 2\/(profile-\w{4})\/\1"$/);
  assert.match(lines[29], /server at ip-\w{4} answered$/);
  assert.match(lines[30], /auth NAME 'player-\w{4}' UID '' MID '\$024e564cxxxx' MAC '\$024e564cxxxx' VERS 'pc\/1\.2/);
  assert.equal(lines[31], '[1!] [thread] end tid=500 (returned, eax=0x0)', 'a number is a number, whoever is called 500');
  assert.equal(lines[32], '[app] settings.ini:3: roam -> NFSU2_ROAM=1');
  assert.match(lines[33], /JOIN: player-\w{4} joined \(peer 9\)$/);
  assert.equal(lines[34], '  --- native state applier (12 types 7/3/1, 0 left)');
});

test('messages between friends: the friend\'s name goes, the rest stays', () => {
  const lines = new Privacy('salt').scrub(`[msg] from Night Owl
[msg] from Night Owl, on the start page
[msg] sending to Night Owl (12 characters)
[msg] to Night Owl: kept
[msg] to Night Owl from the start page: sent
[msg] to Night Owl failed: TypeError: Failed to fetch
`).split('\n');
  const owl = /^\[msg\] from (player-\w{4})$/.exec(lines[0])?.[1];
  assert.ok(owl, lines[0]);
  assert.deepEqual(lines.slice(1, 6), [`[msg] from ${owl}, on the start page`, `[msg] sending to ${owl} (12 characters)`, `[msg] to ${owl}: kept`,
    `[msg] to ${owl} from the start page: sent`, `[msg] to ${owl} failed: TypeError: Failed to fetch`]);
});

test('a stand-in holds in every log of a browser, and differs between browsers', () => {
  const a = new Privacy('one').scrub(LOG), b = new Privacy('one').scrub(LOG), c = new Privacy('two').scrub(LOG);
  assert.equal(a, b);
  assert.notEqual(a, c);
});

test('a log without names comes out as it went in', () => {
  const plain = `=== NFSU2 web, 2026-10-26T10:00:00.000Z, game abc1234, server remote ===
[app] device: screen 390x844 at 3x, 4 cores, touch, Home Screen app
[0!] dispatch: 19458 functions registered (0 duplicates, 0 out of range), table 14.0 MB
[4!] [status] f=3600 t=60.0s fps=60.0
[4!] TRAP in thread 3: RuntimeError: memory access out of bounds
    at nfsu2.wasm.sub_00620ae0 (https://nfsu2.xyz/game.wasm?v=abc:wasm-function[1]:0x1b2)
`;
  assert.equal(new Privacy('s').scrub(plain), plain);
});

test('a name stays known after its line left the log, and the kept copy is scrubbed', () => {
  const p = new Privacy('salt');
  const log = new LogRing(4096, false, 1, 64);
  log.onLine = s => p.learn(s);
  log.keep('=== NFSU2 web, a game ===');
  log.add('[4!] [roam] JOIN: Quietstorm joined (peer 7)');
  for (let i = 0; i < 200; i++) log.add(`[4!] [status] f=${i} t=${i}.0s fps=60.0`);
  log.add('[4!] [roam] f=999 slot 1: Quietstorm\'s car removed (gone)');
  const text = log.text();
  assert.ok(!text.includes('JOIN: Quietstorm'), 'the JOIN line was dropped');
  assert.ok(!p.scrub(text).includes('Quietstorm'));
  // the copy a stopped page keeps (lastgame.ts)
  const store = new Map();
  globalThis.localStorage = { getItem: k => store.get(k) ?? null, setItem: (k, v) => store.set(k, String(v)) };
  globalThis.document = { hidden: false };
  const k = new LastGameKeeper('save', log, () => {}, t => p.scrub(t));
  k.start('abc1234');
  const kept = JSON.parse(store.get('nfsu2.lastgame.save'));
  k.end(0, 'exit');                       // its timer
  assert.equal(kept.scrubbed, true);
  assert.ok(kept.log.includes("slot 1: player-") && !kept.log.includes('Quietstorm'));
});

test('names the page tells it: a notice\'s friend, the profiles', () => {
  const p = new Privacy('salt');
  p.name('Kite Runner', 'player'); p.name('Player', 'you'); p.name('  ', 'room');
  const out = p.scrub('[notice] Kite Runner left\n[app] Player is the game\'s own default\n');
  assert.match(out, /^\[notice\] player-\w{4} left\n\[app\] Player is the game's own default\n$/);
});
