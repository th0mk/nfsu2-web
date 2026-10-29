// Keyphrase saves in the page (web/src/app/vault.ts):
//   node --test web/test/vault.test.mjs
// - keyphrases: "nfsu2-xyz-" and 8 words of the list, read back in any case and with any separators
// - the key and the token: PBKDF2-SHA-256 as node:crypto makes it; sealed saves open only with their keyphrase
// - packing: a player's folder (a profile, the text files) packs far below the server's 256 KB; the content hash
//   ignores the files' times; the DVD and magazine covers are not kept (a picture of 1 MB each)
// - VaultSync, two browsers on a server that keeps what /vault keeps (crates/vault's rules, in memory): made in one,
//   loaded in the other, each one's saves taken by the other, a conflict either way, the same change on both sides,
//   a browser with no profile left, a torn profile during a game, a version with no profile or an older one (a server
//   gone back), a keyphrase deleted elsewhere or lost on the server (still linked, sent again), the server's refusals;
//   covers staying in the browser that took them, and links from when they were sent
// - the device's own: registry.txt and settings.ini's device settings (settings.ts KeyDef.device) never sent, each
//   browser keeping its own over a sync, also when a version sent before brings another's; links that counted them
// - the LAN server's file (gamefile.bin: the races this browser hosted) never sent, each browser keeping its own; links
//   that counted it (versions 1 to 3); a version past the cap names its largest files in the log
// The real server and two real browsers: web/tools/vault-pair.mjs.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const entry = `export * as V from './web/src/app/vault.ts'; export { WORDS } from './web/src/app/vault-words.ts';
  export * as P from './web/src/app/profile.ts'; export { unzip } from './web/src/app/zip.ts'; export * as S from './web/src/app/settings.ts';`;
const js = (await esbuild.build({ stdin: { contents: entry, resolveDir: ROOT, loader: 'ts' }, bundle: true, format: 'esm', write: false, platform: 'neutral' })).outputFiles[0].text;
const { V, WORDS, P, unzip, S } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

const enc = new TextEncoder(), dec = new TextDecoder();
const DIR = 'NFS Underground 2', B = 36;

/** a profile as the game saves one (as far as the page reads it), sealed; `stale`: its checksums do not hold */
function profile(name, { money = 1000, stale = false } = {}) {
  const f = Buffer.alloc(54966);
  f.write('20CM', 0, 'latin1');
  f.writeUInt32LE(54966, 4); f.writeUInt32LE(8, 8); f.writeUInt32LE(54930, 12);
  f.writeUInt32LE(0x10D, 28); f.writeUInt32LE(54930, 32);
  f.write(name, B + 53761, 'latin1');
  f.writeUInt32LE(money, 0xA16A);
  if (!stale) {
    f.writeUInt32LE(P.checksum(f.subarray(28, 36)), 16); f.writeUInt32LE(P.checksum(f.subarray(36)), 20); f.writeUInt32LE(P.checksum(f.subarray(0, 24)), 24);
  }
  return new Uint8Array(f.buffer, f.byteOffset, f.length);
}
const cover = () => { const d = new Uint8Array(1048612); d.set(enc.encode('20CM')); return d; };
const at = n => `${DIR}/${n}/${n}`;
/** the LAN server's file (server.dll's gamefile.bin), as one race leaves it; n: which */
const gamefile = n => { const d = new Uint8Array(10751); d.set([0x3e, 0x9a, 0x02, 0x00, 0, 0, 0, n]); return d; };

test('keyphrases: made from the list, read back however they are typed', () => {
  assert.equal(WORDS.length, 1296);
  assert.equal(new Set(WORDS).size, 1296);
  assert.ok(WORDS.every(w => /^[a-z]{3,5}$/.test(w)));
  const seen = new Set();
  for (let i = 0; i < 500; i++) {
    const p = V.newPhrase();
    const w = p.split('-');
    assert.deepEqual(w.slice(0, 2), ['nfsu2', 'xyz']);
    assert.equal(w.length, 10);
    assert.ok(w.slice(2).every(x => WORDS.includes(x)), p);
    assert.deepEqual(V.readPhrase(p), { phrase: p });
    seen.add(p);
  }
  assert.equal(seen.size, 500);
  const p = 'nfsu2-xyz-acid-acorn-acre-acts-afar-affix-aged-agent';
  for (const typed of [p, p.toUpperCase(), ' acid acorn acre acts afar affix aged agent ', 'NFSU2.xyz: acid,acorn, acre;acts afar\naffix aged agent', 'nfsu2 xyz acid-acorn-acre-acts-afar-affix-aged-agent'])
    assert.deepEqual(V.readPhrase(typed), { phrase: p }, typed);
  assert.match(V.readPhrase('').bad, /Type or paste/);
  assert.match(V.readPhrase('nfsu2-xyz-acid-acorn-acre-acts-afar-affix-aged').bad, /8 words .* this has 7/);
  assert.match(V.readPhrase(p + '-agent').bad, /this has 9/);
  assert.match(V.readPhrase('nfsu2-xyz-acid-acorn-acre-acts-afar-affix-aged-agnet').bad, /"agnet" is not a passphrase word/);
});

test('the key and token: PBKDF2-SHA-256, 100,000 rounds; sealed saves open with their keyphrase only', async () => {
  const p = 'nfsu2-xyz-acid-acorn-acre-acts-afar-affix-aged-agent';
  const k = await V.keysOf(p);
  const bits = crypto.pbkdf2Sync(p, 'nfsu2-xyz keyphrase saves v1', 100000, 64, 'sha256');
  assert.equal(k.token, bits.subarray(32).toString('base64url'));
  assert.match(k.token, /^[A-Za-z0-9_-]{43}$/);
  const plain = enc.encode('the saves');
  const box = await V.seal(k.key, plain);
  assert.equal(box[0], 1);
  assert.equal(box.length, 1 + 12 + plain.length + 16);
  // the key is the first half: AES-GCM as node:crypto does it
  const d = crypto.createDecipheriv('aes-256-gcm', bits.subarray(0, 32), box.subarray(1, 13));
  d.setAAD(Buffer.from('nfsu2-xyz keyphrase saves v1'));
  d.setAuthTag(box.subarray(box.length - 16));
  assert.equal(Buffer.concat([d.update(box.subarray(13, box.length - 16)), d.final()]).toString(), 'the saves');
  assert.deepEqual(await V.unseal(k.key, box), plain);
  const other = await V.keysOf('nfsu2-xyz-acid-acorn-acre-acts-afar-affix-aged-agile');
  await assert.rejects(V.unseal(other.key, box), /do not open with this passphrase/);
  const bent = box.slice(); bent[20] ^= 1;
  await assert.rejects(V.unseal(k.key, bent), /do not open/);
  const v2 = box.slice(); v2[0] = 2;
  await assert.rejects(V.unseal(k.key, v2), /not a passphrase's saves/);
  // two seals of the same saves differ (a new IV each time)
  assert.notDeepEqual(await V.seal(k.key, plain), box);
});

test('a player\'s folder packs far below what the server takes; the hash is of the content', async () => {
  const files = [
    { path: at('THOM'), data: profile('THOM'), mtime: 1 },
    { path: `${DIR}/THOM DVD 1/THOM DVD 1`, data: cover(), mtime: 2 },
    { path: `${DIR}/THOM Magazine 1/THOM Magazine 1`, data: cover(), mtime: 3 },
    { path: 'registry.txt', data: enc.encode('[HKEY]\nx=1\n'.repeat(30)), mtime: 4 },
    { path: 'settings.ini', data: enc.encode('# settings\n#fps = 60\n'.repeat(200)), mtime: 5 },
  ];
  const packed = await V.pack(files);
  assert.ok(packed.length < 16 << 10, `${packed.length} bytes`);
  const back = await unzip(await V.unpack(packed));
  assert.deepEqual(back.map(e => [e.name, e.data.length]), files.map(f => [f.path, f.data.length]));
  assert.deepEqual(back[0].data, files[0].data);
  const h = await V.contentHash(files);
  assert.equal(await V.contentHash(files.map(f => ({ ...f, mtime: 99 }))), h, 'times do not count');
  assert.equal(await V.contentHash([...files].reverse()), h, 'nor the order');
  assert.notEqual(await V.contentHash(files.map((f, i) => i === 3 ? { ...f, data: enc.encode('changed') } : f)), h);
  assert.notEqual(await V.contentHash(files.map((f, i) => i === 3 ? { ...f, path: 'registry2.txt' } : f)), h);
  assert.deepEqual(V.profilesIn(files), ['THOM']);
  assert.ok(V.keeps('settings.ini') && V.keeps(at('THOM')) && !V.keeps('nfsu2000.tga') && !V.keeps('autotest-script.txt'));
  assert.ok(!V.keeps('registry.txt') && V.keptBy(1)('registry.txt') && V.keptBy(1)(`${DIR}/THOM DVD 1/THOM DVD 1`));
  assert.ok(V.keptBy(2)('registry.txt') && !V.keptBy(2)(`${DIR}/THOM DVD 1/THOM DVD 1`) && !V.keptBy(2)('nfsu2000.tga'));
  assert.ok(!V.keptBy(3)('registry.txt') && !V.keptBy(3)(`${DIR}/THOM DVD 1/THOM DVD 1`) && V.keptBy(3)(at('THOM')));
  assert.ok(!V.keeps('gamefile.bin') && V.keptBy(1)('gamefile.bin') && V.keptBy(2)('gamefile.bin') && V.keptBy(3)('gamefile.bin'));
  assert.ok(!V.keptBy(4)('gamefile.bin') && !V.keptBy(4)('registry.txt') && V.keptBy(4)('settings.ini') && V.keptBy(4)(at('THOM')));
  assert.ok(!V.keeps(`${DIR}/THOM DVD 1/THOM DVD 1`) && !V.keeps(`${DIR}/THOM Magazine 12/THOM Magazine 12`));
  assert.ok(V.keeps(`${DIR}/THOM/GARAGE.DAT`) && V.keeps(`${DIR}/THOM/GARAGE.OLD`), 'the garage\'s slots 6-10 go with their profile (runtime/garage.c)');
  assert.ok(V.isCover(`${DIR}/THOM DVD 1/THOM DVD 1`) && !V.isCover(at('THOM')) && !V.isCover('THOM DVD 1'));
  await assert.rejects(V.unpack(enc.encode('not deflate')), /damaged/);
});

// ---- two browsers and a server ----

const DAY = 86400000;

/** /vault as the Rust server answers it (crates/vault, src/app/vault.rs), in memory */
function server() {
  const kept = new Map();
  const s = { now: Date.parse('2026-10-22T12:00:00Z'), kept, refuse: 0, off: false };
  const tag = n => `"${n.toString(16).padStart(16, '0')}"`;
  let revs = 1;
  const head = (v, earlier) => ({ etag: tag(v.rev), 'x-vault-saved': String(v.saved), ...(earlier ? { 'x-vault-earlier': String(earlier.saved) } : {}) });
  s.fetch = async (url, init = {}) => {
    const h = Object.fromEntries(Object.entries(init.headers ?? {}).map(([k, v]) => [k.toLowerCase(), v]));
    const token = (h.authorization ?? '').replace(/^Bearer /, '');
    if (s.off) return new Response('Not found', { status: 404 });            // --vault-gb 0
    if (!token) return new Response('A keyphrase\'s token is needed', { status: 401 });
    if (s.refuse) return new Response('no', { status: s.refuse });
    const k = kept.get(token), method = init.method ?? 'GET';
    if (method === 'GET') {
      const v = url.endsWith('?earlier') ? k?.earlier : k?.cur;
      if (!v) return new Response('Not found', { status: 404 });
      if (!url.endsWith('?earlier') && h['if-none-match'] === tag(v.rev)) return new Response(null, { status: 304, headers: head(v, k.earlier) });
      return new Response(v.body, { status: 200, headers: head(v, url.endsWith('?earlier') ? null : k.earlier) });
    }
    if (method === 'PUT') {
      if (h['if-none-match'] === '*' ? k : !k || h['if-match'] !== tag(k.cur.rev))
        return new Response('Changed elsewhere', { status: 412, headers: k ? head(k.cur, k.earlier) : {} });
      const body = new Uint8Array(init.body);
      if (body.length > 256 << 10) return new Response('Too large', { status: 413 });
      const v = { rev: revs++, saved: s.now, body };
      const n = !k ? { cur: v, earlier: null }
        : Math.floor(k.cur.saved / DAY) !== Math.floor(s.now / DAY) ? { cur: v, earlier: k.cur } : { cur: v, earlier: k.earlier };
      kept.set(token, n);
      return new Response('Saved', { status: k ? 200 : 201, headers: head(v, n.earlier) });
    }
    if (method === 'DELETE') { const had = kept.delete(token); return new Response(had ? 'Deleted' : 'Not found', { status: had ? 200 : 404 }); }
    return new Response('no', { status: 405 });
  };
  return s;
}

/** a browser: its save folder (path -> bytes), its localStorage link, and its VaultSync */
function browser(srv, link = null) {
  const b = { files: new Map(), link, taken: 0, logs: [] };
  b.deps = {
    // as vault-ui.ts files
    files: async (o = {}) => V.kept([...b.files].map(([path, data]) => ({ path, data })), o.was),
    // as saves-ui.ts readVault and restoreVault: what a keyphrase keeps comes in (a cover in it is left out, and so are
    // registry.txt, gamefile.bin and the device's settings); the covers of the profiles it brings stay, registry.txt
    // and gamefile.bin stay, and settings.ini keeps the device's settings here
    restore: async zip => {
      const entries = V.kept((await unzip(zip)).map(e => ({ path: e.name, data: e.data }))), names = new Set(V.profilesIn(entries));
      const ini = entries.find(e => e.path === 'settings.ini'), mine = b.files.get('settings.ini');
      const settings = S.withDevice(ini ? dec.decode(ini.data) : null, mine ? dec.decode(mine) : null);
      for (const p of [...b.files.keys()]) if (V.keeps(p) || V.isCover(p) && !names.has(P.coverOf(p.split('/')[1]).profile)) b.files.delete(p);
      for (const e of entries) if (e !== ini) b.files.set(e.path, e.data);
      if (settings !== null) b.files.set('settings.ini', enc.encode(settings));
    },
    load: () => b.link && JSON.parse(b.link),
    save: l => { b.link = l && JSON.stringify(l); },
    fetch: srv.fetch,
    log: s => b.logs.push(s),
  };
  b.sync = new V.VaultSync(b.deps);
  b.sync.onTaken = () => b.taken++;
  b.money = n => new DataView(b.files.get(n).buffer, b.files.get(n).byteOffset).getUint32(0xA16A, true);
  return b;
}

/** b's saves: one profile with this much money, its settings */
function seed(b, money, ini = '# settings\n') {
  b.files.set(at('THOM'), profile('THOM', { money }));
  b.files.set('settings.ini', enc.encode(ini));
  b.files.set('nfsu2000.tga', new Uint8Array(100));        // a screenshot: never sent
}

test('two browsers on one keyphrase: made, loaded, synced both ways', async () => {
  const srv = server(), a = browser(srv), b = browser(srv);
  seed(a, 1000);
  const phrase = await a.sync.create();
  assert.match(phrase, /^nfsu2-xyz(-[a-z]{3,5}){8}$/);
  assert.equal(a.sync.state.s, 'synced');
  assert.equal(srv.kept.size, 1);
  // the server holds neither the keyphrase nor a save: only sealed bytes under the token
  const [[token, held]] = srv.kept;
  assert.equal(token, (await V.keysOf(phrase)).token);
  assert.ok(!Buffer.from(held.cur.body).includes('THOM') && !Buffer.from(held.cur.body).includes('settings'));
  // b: an unknown keyphrase, then a's
  assert.equal(await b.sync.peek('nfsu2-xyz-acid-acorn-acre-acts-afar-affix-aged-agent'), null);
  const got = await b.sync.peek(phrase);
  assert.deepEqual((await unzip(got.zip)).map(e => e.name).sort(), [at('THOM'), 'settings.ini']);
  b.files.set('nfsu2001.tga', new Uint8Array(5));
  for (const e of await unzip(got.zip)) b.files.set(e.name, e.data);
  await b.sync.adopt(phrase, got.remote);
  assert.equal(b.sync.state.s, 'synced');
  assert.equal(b.money(at('THOM')), 1000);
  // nothing changed: nothing sent, nothing taken
  const rev = srv.kept.get(token).cur.rev;
  await b.sync.push(); await a.sync.pull(); await b.sync.pull();
  assert.equal(srv.kept.get(token).cur.rev, rev);
  // a plays: b takes it (b's screenshot stays)
  a.files.set(at('THOM'), profile('THOM', { money: 2000 }));
  await a.sync.push();
  await b.sync.pull();
  assert.equal(b.money(at('THOM')), 2000);
  assert.equal(b.taken, 1);
  assert.ok(b.files.has('nfsu2001.tga'));
  // b plays: a takes it
  b.files.set(at('THOM'), profile('THOM', { money: 3000 }));
  await b.sync.push();
  await a.sync.pull();
  assert.equal(a.money(at('THOM')), 3000);
  assert.equal(a.sync.state.s, 'synced');
});

test('a conflict, settled either way; the same change on both sides is none', async () => {
  const srv = server(), a = browser(srv), b = browser(srv);
  seed(a, 1000);
  const phrase = await a.sync.create();
  const got = await b.sync.peek(phrase);
  for (const e of await unzip(got.zip)) b.files.set(e.name, e.data);
  await b.sync.adopt(phrase, got.remote);
  // both play
  a.files.set(at('THOM'), profile('THOM', { money: 5000 }));
  b.files.set(at('THOM'), profile('THOM', { money: 7000 }));
  await a.sync.push();
  await b.sync.push();
  assert.equal(b.sync.state.s, 'conflict');
  assert.equal(b.sync.state.why, 'both');
  assert.equal(b.sync.state.theirs.saved, srv.now);
  // while it waits for the player, nothing syncs by itself
  await b.sync.pull(); await b.sync.push();
  assert.equal(b.money(at('THOM')), 7000);
  // b keeps its own: a takes it
  await b.sync.keepMine();
  assert.equal(b.sync.state.s, 'synced');
  await a.sync.pull();
  assert.equal(a.money(at('THOM')), 7000);
  // again, and a takes the other's this time
  a.files.set(at('THOM'), profile('THOM', { money: 8000 }));
  b.files.set(at('THOM'), profile('THOM', { money: 9000 }));
  await b.sync.push();
  await a.sync.pull();
  assert.equal(a.sync.state.s, 'conflict');
  await a.sync.takeTheirs();
  assert.equal(a.money(at('THOM')), 9000);
  assert.equal(a.sync.state.s, 'synced');
  // both pages bring settings.ini up to date the same way: no conflict
  const up = enc.encode('# settings\n#new = 1\n');
  a.files.set('settings.ini', up); b.files.set('settings.ini', up);
  await a.sync.push();
  await b.sync.pull();
  assert.equal(b.sync.state.s, 'synced');
  assert.equal(b.sync.link.rev, a.sync.link.rev);
});

test('a send the server kept but never answered is this browser\'s own: no conflict (issue 33)', async () => {
  const srv = server(), a = browser(srv), b = browser(srv);
  seed(a, 1000);
  const phrase = await a.sync.create();
  const got = await b.sync.peek(phrase);
  for (const e of await unzip(got.zip)) b.files.set(e.name, e.data);
  await b.sync.adopt(phrase, got.remote);
  // the server keeps what x sends, but its answer never comes (a timeout, a frozen page)
  const lost = async (u, i) => { const r = await srv.fetch(u, i); if (i?.method === 'PUT') throw new TypeError('fetch failed'); return r; };
  const save = async (x, money, fetch = srv.fetch) => {
    srv.now += 60_000;
    x.files.set(at('THOM'), profile('THOM', { money }));
    x.deps.fetch = fetch;
    try { await x.sync.push(); } finally { x.deps.fetch = srv.fetch; }
  };
  const reloaded = x => { const y = browser(srv, x.link); y.files = x.files; return y; };
  const rev = () => [...srv.kept.values()][0].cur.rev;
  await assert.rejects(save(a, 2000, lost));
  assert.equal(a.sync.state.code, 'VAULT-NO-ANSWER');
  assert.equal(a.sync.link.sent.length, 1);
  // the next save goes over it: no conflict, and nothing left unanswered
  await save(a, 3000);
  assert.equal(a.sync.state.s, 'synced');
  assert.equal(a.sync.link.sent, undefined);
  await b.sync.pull();
  assert.equal(b.money(at('THOM')), 3000);
  // twice unanswered (the second over the first), then the page reloaded with a save since: it follows its own and sends
  await assert.rejects(save(a, 4000, lost));
  await assert.rejects(save(a, 5000, lost));
  a.files.set(at('THOM'), profile('THOM', { money: 6000 }));
  const a2 = reloaded(a);
  await a2.sync.pull();
  assert.equal(a2.sync.state.s, 'synced');
  await a2.sync.push();
  assert.equal(a2.sync.state.s, 'synced');
  await b.sync.pull();
  assert.equal(b.money(at('THOM')), 6000);
  // unanswered, and the same files kept since: linked at the next look, nothing sent again
  await assert.rejects(save(a2, 7000, lost));
  const kept = rev(), a3 = reloaded(a2);
  await a3.sync.pull(); await a3.sync.push();
  assert.equal(a3.sync.state.s, 'synced');
  assert.equal(rev(), kept);
  // unanswered, then the files went back to what was synced: sent again, so the server does not keep the other
  const back = a3.files.get(at('THOM'));
  await assert.rejects(save(a3, 7500, lost));
  a3.files.set(at('THOM'), back);
  await a3.sync.push();
  assert.equal(a3.sync.state.s, 'synced');
  assert.equal(a3.sync.link.sent, undefined);
  await b.sync.pull();
  assert.equal(b.money(at('THOM')), 7000);
  // another browser's version is still a conflict, with both sides' profiles
  await save(b, 8000);
  const theirs = rev();
  await assert.rejects(save(a3, 9000, lost));
  assert.equal(rev(), theirs);                                    // the server refused it: b's is there
  await save(a3, 10000);
  assert.deepEqual([a3.sync.state.s, a3.sync.state.why], ['conflict', 'both']);
  const st = a3.sync.state;
  assert.deepEqual([st.here.profiles, st.cloud.profiles, st.cloud.saved], [[{ name: 'THOM', money: 10000 }], [{ name: 'THOM', money: 8000 }], st.theirs.saved]);
  assert.equal(a3.money(at('THOM')), 10000);
});

test('no profile here is never sent by itself; a torn profile waits; the copy of an earlier day', async () => {
  const srv = server(), a = browser(srv);
  seed(a, 1000);
  await a.sync.create();
  // the game is writing the profile while it is read: checksums that do not hold
  a.files.set(at('THOM'), profile('THOM', { money: 1500, stale: true }));
  const rev = a.sync.link.rev;
  await a.sync.push({ playing: true });
  assert.equal(a.sync.link.rev, rev, 'not sent during a game');
  a.files.set(at('THOM'), profile('THOM', { money: 1500 }));
  await a.sync.push({ playing: true });
  assert.notEqual(a.sync.link.rev, rev);
  // the next day: the version of the day before is the earlier copy
  srv.now += DAY;
  a.files.set(at('THOM'), profile('THOM', { money: 2500 }));
  await a.sync.push();
  assert.equal(a.sync.link.earlier, srv.now - DAY);
  const e = await a.sync.peek(a.sync.link.phrase, true);
  const prof = (await unzip(e.zip)).find(x => x.name === at('THOM')).data;
  assert.equal(new DataView(prof.buffer, prof.byteOffset).getUint32(0xA16A, true), 1500);
  // the profiles gone here (cleared storage): nothing sent; the keyphrase's come back
  a.files.delete(at('THOM'));
  await a.sync.push();
  assert.deepEqual([a.sync.state.s, a.sync.state.why], ['conflict', 'empty']);
  await a.sync.takeTheirs();
  assert.equal(a.money(at('THOM')), 2500);
  // or, sent anyway
  a.files.delete(at('THOM'));
  await a.sync.push();
  await a.sync.keepMine();
  assert.equal(a.sync.state.s, 'synced');
  assert.equal(a.sync.link.profiles, 0);
});

test('deleted elsewhere, and the server\'s refusals', async () => {
  const srv = server(), a = browser(srv), b = browser(srv);
  seed(a, 1000);
  const phrase = await a.sync.create();
  const got = await b.sync.peek(phrase);
  for (const e of await unzip(got.zip)) b.files.set(e.name, e.data);
  await b.sync.adopt(phrase, got.remote);
  srv.refuse = 429;
  a.files.set(at('THOM'), profile('THOM', { money: 1 }));
  await assert.rejects(a.sync.push());
  // the message in English, the same as written for the page's language, and its code (the same in every language)
  const err = (msg, code, v) => ({ s: 'error', msg, why: { t: msg, v }, code });
  assert.deepEqual(a.sync.state, err('the server asks this address to wait a minute (too many requests)', 'VAULT-HTTP-429', { status: 429 }));
  srv.refuse = 507;
  await assert.rejects(a.sync.push());
  assert.equal(a.sync.state.msg, 'the server has no room for more saves now');
  assert.equal(a.sync.state.code, 'VAULT-HTTP-507');
  srv.refuse = 0;
  await a.sync.push();
  assert.equal(a.sync.state.s, 'synced');
  // b deletes it: a learns at its next sync, and keeps its saves
  await b.sync.remove();
  assert.equal(b.sync.link, null);
  assert.equal(b.link, null);
  assert.equal(srv.kept.size, 0);
  await a.sync.pull();
  assert.equal(a.sync.state.s, 'gone');
  assert.equal(a.money(at('THOM')), 1);
  // a keeps the keyphrase and sends nothing by itself, after a reload too
  assert.equal(a.sync.link.phrase, phrase);
  assert.equal(a.sync.link.gone, true);
  a.files.set(at('THOM'), profile('THOM', { money: 2 }));
  await a.sync.push();
  assert.equal(srv.kept.size, 0);
  const a2 = browser(srv, a.link); a2.files = a.files;          // a reloaded
  assert.equal(a2.sync.state.s, 'gone');
  // the player sends them again: there under the same keyphrase, a syncs again
  await a.sync.resend();
  assert.equal(a.sync.state.s, 'synced');
  assert.ok(!a.sync.link.gone);
  assert.equal(srv.kept.size, 1);
  // a server whose folder came back: a browser marked gone syncs again by itself
  await a2.sync.pull();
  assert.equal(a2.sync.state.s, 'synced');
  assert.ok(!a2.sync.link.gone);
  // a server that keeps no keyphrase saves now (--vault-gb 0): a failure, the link as it was
  srv.off = true;
  await assert.rejects(a.sync.pull());
  assert.deepEqual(a.sync.state, err('the server keeps no cloud saves now', 'VAULT-OFF'));
  assert.equal(a.sync.link.phrase, phrase);
  assert.ok(!a.sync.link.gone);
  srv.off = false;
  await a.sync.pull();
  assert.equal(a.sync.state.s, 'synced');
  // a server that does not answer
  const c = browser({ fetch: async () => { throw new TypeError('fetch failed'); } });
  seed(c, 1);
  await assert.rejects(c.sync.create());
  assert.deepEqual(c.sync.state, err('the server does not answer', 'VAULT-NO-ANSWER'));
  assert.equal(c.sync.link, null);
});

test('a version older than the one here, or with no profile, never replaces the saves here by itself', async () => {
  const srv = server(), a = browser(srv), b = browser(srv);
  seed(a, 1000);
  const phrase = await a.sync.create();
  const got = await b.sync.peek(phrase);
  for (const e of await unzip(got.zip)) b.files.set(e.name, e.data);
  await b.sync.adopt(phrase, got.remote);
  const token = (await V.keysOf(phrase)).token, backup = srv.kept.get(token);
  // a plays on: b takes it
  srv.now += 60_000;
  a.files.set(at('THOM'), profile('THOM', { money: 5000 }));
  await a.sync.push();
  await b.sync.pull();
  assert.equal(b.money(at('THOM')), 5000);
  // the server goes back (its folder restored from a backup): b keeps its saves and asks
  srv.kept.set(token, backup);
  await b.sync.pull();
  assert.deepEqual([b.sync.state.s, b.sync.state.why], ['conflict', 'older']);
  assert.equal(b.money(at('THOM')), 5000);
  await b.sync.keepMine();
  assert.equal(b.sync.state.s, 'synced');
  await a.sync.pull();
  assert.equal(a.sync.state.s, 'synced');
  assert.equal(a.money(at('THOM')), 5000);
  // a keeps its empty saves over the cloud's: b, unchanged since, keeps its profile and asks
  srv.now += 60_000;
  a.files.delete(at('THOM'));
  await a.sync.push();
  assert.deepEqual([a.sync.state.s, a.sync.state.why], ['conflict', 'empty']);
  await a.sync.keepMine();
  await b.sync.pull();
  assert.deepEqual([b.sync.state.s, b.sync.state.why], ['conflict', 'bare']);
  assert.equal(b.money(at('THOM')), 5000);
  await b.sync.takeTheirs();
  assert.ok(!b.files.has(at('THOM')));
});

test('covers stay in the browser that took them; a version with covers (sent before) brings none', async () => {
  const srv = server(), a = browser(srv), b = browser(srv);
  seed(a, 1000);
  const dvd = n => `${DIR}/THOM DVD ${n}/THOM DVD ${n}`;
  a.files.set(dvd(1), new Uint8Array(crypto.randomBytes(1048612)));         // a picture: packs to as much
  const phrase = await a.sync.create();
  const token = (await V.keysOf(phrase)).token;
  assert.ok(srv.kept.get(token).cur.body.length < 16 << 10);
  const got = await b.sync.peek(phrase);
  assert.deepEqual((await unzip(got.zip)).map(e => e.name).sort(), [at('THOM'), 'settings.ini']);
  for (const e of await unzip(got.zip)) b.files.set(e.name, e.data);
  await b.sync.adopt(phrase, got.remote);
  // b's own cover of THOM, and one of a profile b no longer has
  b.files.set(dvd(2), cover());
  b.files.set(`${DIR}/GONE DVD 1/GONE DVD 1`, cover());
  // a cover shoot changes no file a keyphrase keeps: nothing sent
  const rev = srv.kept.get(token).cur.rev;
  a.files.set(dvd(3), cover());
  await a.sync.push();
  assert.equal(srv.kept.get(token).cur.rev, rev);
  // a plays: b takes it, its cover of THOM stays, the other one goes
  a.files.set(at('THOM'), profile('THOM', { money: 2000 }));
  await a.sync.push();
  await b.sync.pull();
  assert.equal(b.money(at('THOM')), 2000);
  assert.ok(b.files.has(dvd(2)) && !b.files.has(dvd(1)) && !b.files.has(`${DIR}/GONE DVD 1/GONE DVD 1`));
  assert.ok(a.files.has(dvd(1)) && a.files.has(dvd(3)));
  // a version sent before the covers stayed out, with a (black) cover: b takes it, without the cover, and the hash
  // leaves it out (b is synced with it, so b sends nothing)
  srv.now += 60_000;
  const old = [{ path: at('THOM'), data: profile('THOM', { money: 3000 }) }, { path: dvd(1), data: cover() },
    { path: 'settings.ini', data: enc.encode('# settings\n') }];
  const k = await V.keysOf(phrase), etag = `"${srv.kept.get(token).cur.rev.toString(16).padStart(16, '0')}"`;
  assert.ok('saved' in await new V.VaultApi(k.token, srv.fetch).put(await V.seal(k.key, await V.pack(old)), etag));
  await b.sync.pull();
  assert.equal(b.money(at('THOM')), 3000);
  assert.ok(b.files.has(dvd(2)) && !b.files.has(dvd(1)));
  const rev2 = srv.kept.get(token).cur.rev;
  await b.sync.push();
  assert.equal(srv.kept.get(token).cur.rev, rev2);
  assert.equal(b.sync.state.s, 'synced');
});

test('a link from when covers were sent: unchanged files still count as unchanged, changed ones as changed', async () => {
  const srv = server(), a = browser(srv);
  seed(a, 1000);
  const phrase = await a.sync.create();
  const dvd = `${DIR}/THOM DVD 1/THOM DVD 1`;
  /** b, synced with a's version as a link from before 2026-10-25 kept it: no v, the hash over the covers too */
  async function before(change) {
    const b = browser(srv), got = await b.sync.peek(phrase);
    for (const e of await unzip(got.zip)) b.files.set(e.name, e.data);
    b.files.set(dvd, cover());
    await b.sync.adopt(phrase, got.remote);
    const l = JSON.parse(b.link);
    delete l.v;
    l.hash = await V.contentHash([...b.files].filter(([p]) => !p.endsWith('.tga')).map(([path, data]) => ({ path, data })));
    const b2 = browser(srv, JSON.stringify(l)); b2.files = b.files;      // b reloaded with the new page
    change?.(b2);
    return b2;
  }
  // b unchanged since; a sends a new version: b takes it, no question
  const b = await before();
  srv.now += 60_000;
  a.files.set(at('THOM'), profile('THOM', { money: 2000 }));
  await a.sync.push();
  await b.sync.pull();
  assert.equal(b.sync.state.s, 'synced');
  assert.equal(b.money(at('THOM')), 2000);
  assert.ok(b.files.has(dvd));
  assert.equal(b.sync.link.v, 4);
  // b unchanged since and nothing new: it sends nothing
  const c = await before(), rev = srv.kept.get((await V.keysOf(phrase)).token).cur.rev;
  await c.sync.pull(); await c.sync.push();
  assert.equal(srv.kept.get((await V.keysOf(phrase)).token).cur.rev, rev);
  assert.equal(c.sync.state.s, 'synced');
  // b played since; a sends a new version: both changed, the player picks
  const d = await before(x => x.files.set(at('THOM'), profile('THOM', { money: 9000 })));
  srv.now += 60_000;
  a.files.set(at('THOM'), profile('THOM', { money: 3000 }));
  await a.sync.push();
  await d.sync.pull();
  assert.deepEqual([d.sync.state.s, d.sync.state.why], ['conflict', 'both']);
  assert.equal(d.money(at('THOM')), 9000);
});

// ---- the device's own settings ----

/** a settings.ini as the Settings panel writes one: these keys set, the rest the template's */
const ini = (sets, extra = '') => sets.reduce((t, [k, v]) => S.settingsPut(t, k, v), S.TEMPLATE) + extra;
/** what a settings.ini sets for the game: variable -> value, settings.ini's lines only */
const fromFile = text => { const r = S.resolveEnv(text, {}, {}); return Object.fromEntries(Object.keys(r.byEnv).map(k => [k, r.env[k]])); };

test('settings.ini without the device\'s settings, and with this browser\'s again', () => {
  const dev = S.KEYS.filter(k => k.device).map(k => k.key);
  assert.ok(['render', 'fps', 'resolution', 'fullscreen', 'audio_latency', 'gamepad_deadzone', 'ui_textures', 'seed_registry'].every(k => dev.includes(k)));
  assert.ok(!['roam_name', 'language', 'roam_crew', 'horn', 'movies', 'online', 'hud'].some(k => dev.includes(k)));
  assert.ok(S.KEYS.filter(k => k.device).every(k => k.scope === 'web'));
  // a phone's: its own scale, frame rate and controller, a variable of one by its own name, and a lower-case key of
  // one (pad_deadzone: NFSU2_PAD_DEADZONE); its name and language are the player's
  const phone = ini([['render', 'scale:0.5'], ['fps', '30'], ['roam_name', 'Ann'], ['language', 'german'], ['gamepad_deadzone', '0.2']],
    'NFSU2_FPS = 60\npad_deadzone = 0.3\nNFSU2_STATUS_EVERY = 600\n');
  const kept = S.withoutDevice(phone);
  assert.deepEqual(fromFile(kept), { NFSU2_ROAM_NAME: 'Ann', NFSU2_LANGUAGE: 'german', NFSU2_STATUS_EVERY: '600' });
  assert.match(kept, /^#render = backing$/m);
  assert.match(kept, /^#fps = auto$/m);
  assert.doesNotMatch(kept, /^(NFSU2_FPS|pad_deadzone)/m);
  assert.equal(S.withoutDevice(kept), kept);
  const plain = ini([['roam_name', 'Ann']]);
  assert.equal(S.withoutDevice(plain), plain, 'nothing of the device: the file as it is');
  // a PC's own in the phone's: the PC's scale (its first fps line wins, as in resolveEnv), the phone's name and language
  const pc = ini([['render', 'scale:1.5'], ['fps', '120'], ['roam_name', 'Bob']], 'fps = 240\n');
  const got = S.withDevice(phone, pc);
  assert.deepEqual(fromFile(got), { NFSU2_RENDER: 'scale:1.5', NFSU2_FPS: '120', NFSU2_ROAM_NAME: 'Ann', NFSU2_LANGUAGE: 'german', NFSU2_STATUS_EVERY: '600' });
  assert.match(got, /^render = scale:1\.5$/m, 'in the place of its template line');
  assert.equal(S.withoutDevice(got), kept);
  // none here: the device's settings at their defaults (or this device's: device.ts); neither file: none
  assert.equal(S.withDevice(phone, null), kept);
  assert.equal(S.withDevice(null, null), null);
  assert.equal(S.withDevice(null, plain), null);
  assert.deepEqual(fromFile(S.withDevice(null, pc)), { NFSU2_RENDER: 'scale:1.5', NFSU2_FPS: '120' });
});

test('the graphics options and the device\'s settings stay in each browser', async () => {
  const srv = server(), pc = browser(srv), phone = browser(srv);
  seed(pc, 1000, ini([['render', 'scale:1.5'], ['fps', '120'], ['roam_name', 'Ann']]));
  pc.files.set('registry.txt', enc.encode('d g_RacingResolution=5\n'));
  const phrase = await pc.sync.create();
  const got = await phone.sync.peek(phrase);
  const sent = await unzip(got.zip);
  assert.deepEqual(sent.map(e => e.name).sort(), [at('THOM'), 'settings.ini']);
  assert.deepEqual(fromFile(dec.decode(sent.find(e => e.name === 'settings.ini').data)), { NFSU2_ROAM_NAME: 'Ann' });
  // the phone, with its own, loads them: its own stay
  phone.files.set('settings.ini', enc.encode(ini([['render', 'scale:0.5'], ['fps', '30']])));
  phone.files.set('registry.txt', enc.encode('d g_RacingResolution=1\n'));
  await phone.deps.restore(got.zip);
  await phone.sync.adopt(phrase, got.remote);
  assert.deepEqual(fromFile(dec.decode(phone.files.get('settings.ini'))), { NFSU2_RENDER: 'scale:0.5', NFSU2_FPS: '30', NFSU2_ROAM_NAME: 'Ann' });
  assert.equal(dec.decode(phone.files.get('registry.txt')), 'd g_RacingResolution=1\n');
  // the PC's scale, frame rate and graphics options change: nothing is sent
  const token = (await V.keysOf(phrase)).token, rev = srv.kept.get(token).cur.rev;
  pc.files.set('settings.ini', enc.encode(S.settingsPut(dec.decode(pc.files.get('settings.ini')), 'render', 'backing')));
  pc.files.set('registry.txt', enc.encode('d g_RacingResolution=4\n'));
  await pc.sync.push();
  assert.equal(srv.kept.get(token).cur.rev, rev);
  // its name does: the phone takes it, with its own scale, frame rate and graphics options still
  srv.now += 60_000;
  pc.files.set('settings.ini', enc.encode(S.settingsPut(dec.decode(pc.files.get('settings.ini')), 'roam_name', 'Bob')));
  await pc.sync.push();
  assert.notEqual(srv.kept.get(token).cur.rev, rev);
  await phone.sync.pull();
  assert.equal(phone.sync.state.s, 'synced');
  assert.deepEqual(fromFile(dec.decode(phone.files.get('settings.ini'))), { NFSU2_RENDER: 'scale:0.5', NFSU2_FPS: '30', NFSU2_ROAM_NAME: 'Bob' });
  assert.equal(dec.decode(phone.files.get('registry.txt')), 'd g_RacingResolution=1\n');
  // and the phone's own changes send nothing either
  const rev2 = srv.kept.get(token).cur.rev;
  phone.files.set('settings.ini', enc.encode(S.settingsPut(dec.decode(phone.files.get('settings.ini')), 'fps', '60')));
  await phone.sync.push();
  assert.equal(srv.kept.get(token).cur.rev, rev2);
  // a version sent before, with the other browser's graphics options and device settings: taken without them, and
  // the phone is in sync with it (it sends nothing)
  srv.now += 60_000;
  const old = [{ path: at('THOM'), data: profile('THOM', { money: 3000 }) }, { path: 'registry.txt', data: enc.encode('d g_RacingResolution=5\n') },
    { path: 'settings.ini', data: enc.encode(ini([['render', 'scale:2'], ['roam_name', 'Cy']])) }];
  const k = await V.keysOf(phrase), etag = `"${rev2.toString(16).padStart(16, '0')}"`;
  assert.ok('saved' in await new V.VaultApi(k.token, srv.fetch).put(await V.seal(k.key, await V.pack(old)), etag));
  await phone.sync.pull();
  assert.equal(phone.money(at('THOM')), 3000);
  assert.deepEqual(fromFile(dec.decode(phone.files.get('settings.ini'))), { NFSU2_RENDER: 'scale:0.5', NFSU2_FPS: '60', NFSU2_ROAM_NAME: 'Cy' });
  assert.equal(dec.decode(phone.files.get('registry.txt')), 'd g_RacingResolution=1\n');
  const rev3 = srv.kept.get(token).cur.rev;
  await phone.sync.push();
  assert.equal(srv.kept.get(token).cur.rev, rev3);
  assert.equal(phone.sync.state.s, 'synced');
});

test('links that counted the graphics options, the device\'s settings or the LAN server\'s file (versions 1 to 3): unchanged is unchanged', async () => {
  const srv = server(), a = browser(srv);
  seed(a, 1000);
  const phrase = await a.sync.create();
  const dvd = `${DIR}/THOM DVD 1/THOM DVD 1`;
  /** what a link of version v counted, as the page of that time kept it: 1, all but the screenshots; 2, the covers out
   *  too; 3, registry.txt out too and settings.ini without the device's settings */
  const counted = (v, files) => files.filter(([p]) => !p.endsWith('.tga') && (v < 2 || p !== dvd) && (v < 3 || p !== 'registry.txt'))
    .map(([path, data]) => ({ path, data: v >= 3 && path === 'settings.ini' ? enc.encode(S.withoutDevice(dec.decode(data))) : data }));
  /** b synced with a's version as a link of version v kept it, its own graphics options, scale, cover and LAN server's
   *  file beside */
  async function linkOf(v, change) {
    const b = browser(srv), got = await b.sync.peek(phrase);
    for (const e of await unzip(got.zip)) b.files.set(e.name, e.data);
    b.files.set('settings.ini', enc.encode(S.settingsPut(dec.decode(b.files.get('settings.ini')), 'render', 'scale:0.5')));
    b.files.set('registry.txt', enc.encode('d g_RacingResolution=1\n'));
    b.files.set(dvd, cover());
    b.files.set('gamefile.bin', gamefile(1));
    await b.sync.adopt(phrase, got.remote);
    const l = JSON.parse(b.link);
    if (v === 1) delete l.v; else l.v = v;
    l.hash = await V.contentHash(counted(v, [...b.files]));
    const b2 = browser(srv, JSON.stringify(l)); b2.files = b.files;      // b reloaded with the new page
    change?.(b2);
    return b2;
  }
  const token = (await V.keysOf(phrase)).token, rev = () => srv.kept.get(token).cur.rev;
  const scale = (x, r) => x.files.set('settings.ini', enc.encode(S.settingsPut(dec.decode(x.files.get('settings.ini')), 'render', r)));
  for (const v of [1, 2, 3]) {
    // unchanged since; a sends a new version: b takes it, no question, and keeps its own
    const b = await linkOf(v);
    srv.now += 60_000;
    a.files.set(at('THOM'), profile('THOM', { money: 2000 + v }));
    await a.sync.push();
    await b.sync.pull();
    assert.equal(b.sync.state.s, 'synced', `v${v}`);
    assert.equal(b.money(at('THOM')), 2000 + v);
    assert.equal(b.sync.link.v, 4);
    assert.equal(fromFile(dec.decode(b.files.get('settings.ini'))).NFSU2_RENDER, 'scale:0.5');
    assert.ok(b.files.has('registry.txt') && b.files.has(dvd));
    assert.deepEqual(b.files.get('gamefile.bin'), gamefile(1));
    // unchanged since and nothing new: it sends nothing
    const quiet = await linkOf(v), r0 = rev();
    await quiet.sync.pull(); await quiet.sync.push();
    assert.equal(rev(), r0, `v${v}`);
    assert.equal(quiet.sync.state.s, 'synced');
    // what the link counted changed since (its LAN server's file; before version 3 its scale too): a change, sent
    // once, without them; after that one, none
    for (const changed of v < 3 ? [x => scale(x, 'backing'), x => x.files.set('gamefile.bin', gamefile(2))] : [x => x.files.set('gamefile.bin', gamefile(2))]) {
      const c = await linkOf(v, changed), r1 = rev();
      await c.sync.push();
      assert.notEqual(rev(), r1, `v${v}`);
      const r2 = rev(), k = await V.keysOf(phrase);
      const sent = await unzip(await V.unpack(await V.unseal(k.key, srv.kept.get(token).cur.body)));
      assert.deepEqual(sent.map(e => e.name).sort(), [at('THOM'), 'settings.ini']);
      scale(c, 'scale:2'); c.files.set('gamefile.bin', gamefile(3));
      await c.sync.push();
      assert.equal(rev(), r2, `v${v}`);
      await a.sync.pull();                                                 // a takes what c sent
      assert.equal(a.sync.state.s, 'synced');
    }
    // played since: both changed, the player picks
    const d = await linkOf(v, x => x.files.set(at('THOM'), profile('THOM', { money: 9000 })));
    srv.now += 60_000;
    a.files.set(at('THOM'), profile('THOM', { money: 3000 + v }));
    await a.sync.push();
    await d.sync.pull();
    assert.deepEqual([d.sync.state.s, d.sync.state.why], ['conflict', 'both'], `v${v}`);
  }
});

test('the LAN server\'s file (gamefile.bin) stays in each browser', async () => {
  const srv = server(), pc = browser(srv), phone = browser(srv);
  seed(pc, 1000);
  pc.files.set('gamefile.bin', gamefile(1));
  const phrase = await pc.sync.create();
  const got = await phone.sync.peek(phrase);
  assert.deepEqual((await unzip(got.zip)).map(e => e.name).sort(), [at('THOM'), 'settings.ini']);
  // the phone, with its own, loads them: its own stays
  phone.files.set('gamefile.bin', gamefile(2));
  await phone.deps.restore(got.zip);
  await phone.sync.adopt(phrase, got.remote);
  assert.deepEqual(phone.files.get('gamefile.bin'), gamefile(2));
  // the PC hosts a race: nothing is sent
  const token = (await V.keysOf(phrase)).token, rev = srv.kept.get(token).cur.rev;
  pc.files.set('gamefile.bin', gamefile(3));
  await pc.sync.push();
  assert.equal(srv.kept.get(token).cur.rev, rev);
  // it plays: the phone takes the profile and keeps its own file
  srv.now += 60_000;
  pc.files.set(at('THOM'), profile('THOM', { money: 2000 }));
  await pc.sync.push();
  assert.notEqual(srv.kept.get(token).cur.rev, rev);
  await phone.sync.pull();
  assert.equal(phone.sync.state.s, 'synced');
  assert.equal(phone.money(at('THOM')), 2000);
  assert.deepEqual(phone.files.get('gamefile.bin'), gamefile(2));
  assert.deepEqual(pc.files.get('gamefile.bin'), gamefile(3));
  // a version sent before (by an older page), with the other browser's file: taken without it, and the phone is in
  // sync with it (it sends nothing)
  srv.now += 60_000;
  const rev2 = srv.kept.get(token).cur.rev;
  const old = [{ path: at('THOM'), data: profile('THOM', { money: 3000 }) }, { path: 'gamefile.bin', data: gamefile(4) },
    { path: 'settings.ini', data: enc.encode('# settings\n') }];
  const k = await V.keysOf(phrase), etag = `"${rev2.toString(16).padStart(16, '0')}"`;
  assert.ok('saved' in await new V.VaultApi(k.token, srv.fetch).put(await V.seal(k.key, await V.pack(old)), etag));
  await phone.sync.pull();
  assert.equal(phone.money(at('THOM')), 3000);
  assert.deepEqual(phone.files.get('gamefile.bin'), gamefile(2));
  const rev3 = srv.kept.get(token).cur.rev;
  await phone.sync.push();
  assert.equal(srv.kept.get(token).cur.rev, rev3);
  assert.equal(phone.sync.state.s, 'synced');
});

test('saves past the cap name their largest files in the log; one stuck on the LAN server\'s file sends again', async () => {
  const srv = server(), a = browser(srv);
  seed(a, 1000);
  a.files.set('other.bin', crypto.randomBytes(300 << 10));             // something else in the folder, past the cap
  await assert.rejects(a.sync.create(), e => e.code === 'VAULT-TOO-LARGE');
  assert.equal(a.sync.state.code, 'VAULT-TOO-LARGE');
  assert.equal(a.logs.length, 1);
  assert.match(a.logs[0], /^\[app\] keyphrase: the saves pack into 30\d KB \(3 files, 1 profiles\); the largest packed: other\.bin 30\d\.\d KB, /);
  assert.match(a.logs[0], /NFS Underground 2\/THOM\/THOM \d+\.\d KB/);
  // a browser linked by the page before 2026-10-28 (version 3: its hash over the LAN server's file too), whose file
  // grew past the cap since while it played: its saves go, without the file
  const b = browser(srv);
  seed(b, 1000);
  b.files.set('gamefile.bin', gamefile(1));
  const phrase = await b.sync.create();
  const l = JSON.parse(b.link);
  l.v = 3;
  l.hash = await V.contentHash([...b.files].filter(([p]) => !p.endsWith('.tga')).map(([path, data]) => ({ path, data })));
  const b2 = browser(srv, JSON.stringify(l)); b2.files = b.files;        // b reloaded with the new page
  b2.files.set('gamefile.bin', crypto.randomBytes(300 << 10));
  b2.files.set(at('THOM'), profile('THOM', { money: 5000 }));
  const k = await V.keysOf(phrase), rev = srv.kept.get(k.token).cur.rev;
  await b2.sync.push();
  assert.equal(b2.sync.state.s, 'synced');
  assert.equal(b2.sync.link.v, 4);
  assert.notEqual(srv.kept.get(k.token).cur.rev, rev);
  const sent = await unzip(await V.unpack(await V.unseal(k.key, srv.kept.get(k.token).cur.body)));
  assert.deepEqual(sent.map(e => e.name).sort(), [at('THOM'), 'settings.ini']);
  assert.equal(new DataView(sent.find(e => e.name === at('THOM')).data.buffer, sent.find(e => e.name === at('THOM')).data.byteOffset).getUint32(0xA16A, true), 5000);
  assert.deepEqual(b2.logs, []);
});
