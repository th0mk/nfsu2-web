// The online host's services (web/server/online/), driven by fake game
// clients that speak the switch's frames, as the browsers' runtime does:
//   node --test web/test/online-lobby.test.mjs
// The redirector (@dir to the lobby; no answer to @tic), the login (any name;
// the reply within the game's 122 bytes; PASS, REGKEY and PSES never kept),
// personas (one holder per name, the LKEY the buddy login gets), the config
// blob, rooms and chat (with the flood mute), the game list, a game from
// creation to the race start (+ses with every player's virtual address, the
// same AUTH for all), the countdown of a start before every player is ready
// and the kick of those still not ready, the lobby link closing at the start
// without ending the race, leaving and deleting, the buddy login, and the
// limits.
import test from 'node:test';
import assert from 'node:assert/strict';
import { OnlineHost, PORTS } from '../server/online/index.mjs';
import { VHOST_VIP, lanFrame } from '../server/online/vhost.mjs';
import { encodeFrame, FrameReader, fieldMap, encodeFields } from '../server/online/aries.mjs';

const ip = (s) => s.split('.').reduce((a, b) => ((a << 8) | +b) >>> 0, 0);
const SYN = 1, SYNACK = 2, RST = 3, DATA = 4, FIN = 5;

// one online host and its browsers; frames out are routed to the fake clients by address
function world(opts = {}) {
  let t = 1_000_000;
  const lines = [];
  const host = new OnlineHost({ log: s => lines.push(s), now: () => t, ...opts });
  const clients = new Map();
  host.attachSwitch((vip, f) => { clients.get(vip)?.frame(f); return true; });
  const w = { host, lines, clients, advance: ms => { t += ms; host.lobby.tick(); }, now: () => t };
  w.client = (vipText) => { const c = new Game(w, ip(vipText)); clients.set(c.vip, c); return c; };
  return w;
}
let nextPort = 50000, nextCid = 1;
class Game {
  constructor(w, vip) { this.w = w; this.vip = vip; this.conns = new Map(); }
  frame(f) {
    const port = f.readUInt16BE(14), op = f[16], c = this.conns.get(port);
    if (!c) return;
    if (op === SYNACK) c.open = true;
    else if (op === RST) c.reset = true;
    else if (op === FIN) c.fin = true;
    else if (op === DATA) for (const m of c.r.push(f.subarray(24))) c.msgs.push({ kind: m.kind, code: m.code, f: fieldMap(m.body, []), raw: m.body });
  }
  connect(dport) {
    const c = { port: nextPort++, cid: nextCid++, dport, r: new FrameReader(1 << 16), msgs: [], open: false, reset: false, fin: false };
    this.conns.set(c.port, c);
    this.w.host.frame(this.vip, lanFrame(4, this.vip, VHOST_VIP, c.port, dport, SYN, c.cid));
    c.send = (kind, fields = []) => this.w.host.frame(this.vip, lanFrame(4, this.vip, VHOST_VIP, c.port, dport, DATA, c.cid, encodeFrame(kind, 0, fields)));
    c.close = () => this.w.host.frame(this.vip, lanFrame(4, this.vip, VHOST_VIP, c.port, dport, FIN, c.cid));
    c.take = () => c.msgs.splice(0);
    c.last = kind => c.msgs.filter(m => m.kind === kind).at(-1);
    return c;
  }
  /** a lobby session up to a persona (what the game does from the account screen to the Online Main Menu) */
  login(name, pass = 'secret') {
    const c = this.connect(PORTS.lobby);
    c.send('addr', [['ADDR', '10.77.0.99'], ['PORT', c.port]]);
    c.send('skey', [['SKEY', '$5075626c6963204b6579']]);
    c.send('news', [['NAME', 7]]);
    c.send('sele', [['MYGAME', 1], ['STATS', 5000], ['ASYNC', 1], ['MESGS', 1]]);
    c.send('auth', [['NAME', name], ['PASS', pass], ['REGKEY', 'AAAA-BBBB-CCCC'], ['PSES', '0123456789abcdef'], ['MID', '$024e564c0002']]);
    c.send('pers', [['PERS', name], ['MID', '$024e564c0002']]);
    return c;
  }
}

test('redirector: @dir points at the lobby, @tic gets no answer, then the link closes', () => {
  const w = world(), g = w.client('10.77.0.2');
  const c = g.connect(PORTS.redirector);
  assert.ok(c.open);
  c.send('@tic', 'RC4+MD5-V2');
  assert.equal(c.msgs.length, 0);
  c.send('@dir', [['PROD', 'nfs-pc-2005'], ['MID', '$024e564c0002']]);
  const d = c.last('@dir');
  assert.equal(d.f.get('ADDR'), '10.77.0.1'); assert.equal(d.f.get('PORT'), String(PORTS.lobby));
  assert.match(d.f.get('MASK'), /^[0-9a-f]{32}$/); assert.ok(+d.f.get('SESS') > 0);
  assert.ok(c.fin, 'the redirector closes after its answer');
  const ssl = g.connect(20921);
  assert.ok(ssl.reset, 'no SSL port: refused');
});

test('login: config, any name, personas, LKEY, nothing secret kept', () => {
  const w = world(), g = w.client('10.77.0.2');
  const c = g.login('Speedy', 'CANARY-PASSWORD-123');
  const m = c.take(), kinds = m.map(x => `${x.kind}/${x.code}`);
  assert.deepEqual(kinds.slice(0, 4), ['skey/', 'news/new7', 'sele/', 'auth/']);
  const news = m.find(x => x.kind === 'news').f;
  assert.equal(news.get('BUDDY_SERVER'), '10.77.0.1'); assert.equal(news.get('BUDDY_PORT'), String(PORTS.buddy));
  const auth = m.find(x => x.kind === 'auth');
  assert.equal(auth.f.get('PERSONAS'), 'Speedy'); assert.equal(auth.f.get('ADDR'), '10.77.0.2', 'the virtual address, never another');
  assert.ok(encodeFields([...auth.f]).length <= 122, 'the auth reply fits the game\'s 122 bytes');
  assert.equal(m.filter(x => x.kind === '+rom').length, 7);
  const pers = m.find(x => x.kind === 'pers');
  assert.equal(pers.f.get('PERS'), 'Speedy'); assert.match(pers.f.get('LKEY'), /^[0-9a-f]{30}$/);
  assert.ok(encodeFields([...pers.f]).length <= 108);
  assert.ok(m.some(x => x.kind === '+who') && m.some(x => x.kind === '+sst'));
  // the server keeps nothing of the password, the CD key or the file checksum, and logs none of them
  const sess = [...w.host.lobby.sessions][0];
  const dump = JSON.stringify(sess, (k, v) => (k === 'conn' || k === 'reader' || k === 'msgs' || k === 'chats' ? undefined : v));
  for (const secret of ['CANARY-PASSWORD-123', 'AAAA-BBBB', '0123456789abcdef', '024e564c0002']) {
    assert.ok(!dump.includes(secret), `session keeps ${secret}`);
    assert.ok(!w.lines.join('\n').includes(secret), `log has ${secret}`);
  }
  // one holder per persona name while online (another browser gets it with digits); a bad name is refused
  const g2 = w.client('10.77.0.3'), c2 = g2.login('Speedy');
  assert.match(c2.last('pers').f.get('PERS'), /^Speedy\d{4}$/);
  const c3 = g2.login('x y');
  assert.equal(c3.last('auth').code, 'imst');
  // ... and so is one that reads as a slur (names.mjs), as an account or a persona
  assert.equal(g2.login('F4g_42').last('auth').code, 'imst');
  // a second persona for this session (create, select, delete)
  c.send('cper', [['PERS', 'Second']]);
  assert.equal(c.last('cper').code, ''); assert.equal(c.last('cper').f.get('PERS'), 'Second');
  c.send('cper', [['PERS', 'Speedy']]);
  assert.equal(c.last('cper').code, 'dupl');
  c.send('cper', [['PERS', 'Sp1cs']]);
  assert.equal(c.last('cper').code, 'invp');
  c.send('dper', [['PERS', 'Second']]);
  assert.equal(c.last('dper').f.get('RESULT'), '0');
});

test('rooms and chat: the room hears it, a private message reaches one, a flood is muted', () => {
  const w = world(), a = w.client('10.77.0.2').login('Alpha'), b = w.client('10.77.0.3').login('Bravo');
  a.send('move', [['NAME', 'Lounge']]); b.send('move', [['NAME', 'Lounge']]);
  assert.equal(b.last('move').f.get('COUNT'), '2');
  assert.equal(b.msgs.filter(x => x.kind === '+usr').at(-1).f.get('N'), 'Alpha');
  assert.equal(a.msgs.filter(x => x.kind === '+usr').at(-1).f.get('A'), '10.77.0.3');
  a.take(); b.take();
  a.send('mesg', [['TEXT', 'hello \x9b there\x07']]);
  assert.equal(b.last('+msg').f.get('T'), 'hello there'); assert.equal(b.last('+msg').f.get('N'), 'Alpha');
  b.send('mesg', [['TEXT', 'psst'], ['PRIV', 'alpha']]);
  assert.equal(a.last('+msg').f.get('F'), 'P'); assert.equal(a.last('+msg').f.get('T'), 'psst');
  for (let i = 0; i < 8; i++) a.send('mesg', [['TEXT', `spam ${i}`]]);
  assert.equal(a.last('mesg').code, 'mute');
  assert.ok(w.lines.some(l => l.includes('muted')));
  const heard = b.msgs.filter(x => x.kind === '+msg' && x.f.get('N') === 'Alpha').length;
  assert.ok(heard <= 6, `heard ${heard}`);
  a.send('mesg', [['TEXT', 'x'.repeat(1000)]]);
  b.send('move', [['NAME', '']]);
  assert.equal(a.last('+usr').f.get('T'), '1', 'Bravo left the room');
});

test('a game: create, list, join, ready, start; +ses names every player by virtual address', () => {
  const w = world(), a = w.client('10.77.0.2').login('Alpha'), b = w.client('10.77.0.3').login('Bravo');
  const P = 'TRACK=4123\nDIR=0\nLAPS=2';
  a.send('gcre', [['NAME', '088.Alpha'], ['MAXSIZE', 4], ['MINSIZE', 2], ['CUSTFLAGS', 134217971], ['SYSFLAGS', 0], ['PARAMS', P]]);
  const cre = a.last('gcre');
  assert.equal(cre.code, ''); assert.equal(cre.f.get('HOST'), 'Alpha'); assert.equal(cre.f.get('ADDR0'), '10.77.0.2');
  assert.equal(cre.f.get('PARAMS'), P);
  assert.ok(a.last('+mgm') && a.last('+who').f.get('F') === 'G');
  // the game list: a Filter of another race type lists it too
  b.send('gsea', [['START', 0], ['COUNT', 20], ['CUSTFLAGS', 67109107], ['CUSTMASK', 67109363], ['SYSFLAGS', 0], ['SYSMASK', 786432]]);
  assert.equal(b.last('gsea').f.get('COUNT'), '1'); assert.equal(b.last('+gam').f.get('NAME'), '088.Alpha');
  b.send('gjoi', [['NAME', '088.Alpha']]);
  const j = b.last('gjoi');
  assert.equal(j.f.get('COUNT'), '2'); assert.equal(j.f.get('OPPO1'), 'Bravo'); assert.equal(j.f.get('ADDR1'), '10.77.0.3');
  assert.equal(a.last('+mgm').f.get('COUNT'), '2');
  b.send('mesg', [['TEXT', 'hi room'], ['ATTR', 'G']]);           // the game room's chat
  assert.equal(a.last('+msg').f.get('T'), 'hi room'); assert.equal(a.last('+msg').f.get('N'), 'Bravo');
  b.send('gset', [['NAME', '088.Alpha'], ['USERFLAGS', 134217728]]);
  assert.equal(a.last('+mgm').f.get('OPFLAG1'), '134217728');
  a.send('gset', [['NAME', '088.Alpha'], ['USERFLAGS', 134217728]]);
  b.send('gsta', [['NAME', '088.Alpha']]);
  assert.equal(b.last('gsta').code, 'host', 'only the creator starts');
  a.send('gsta', [['NAME', '088.Alpha']]);
  const sa = a.last('+ses'), sb = b.last('+ses');
  assert.ok(sa && sb);
  assert.equal(sa.f.get('SELF'), 'Alpha'); assert.equal(sb.f.get('SELF'), 'Bravo');
  assert.equal(sa.f.get('SEED'), sb.f.get('SEED')); assert.equal(sa.f.get('AUTH'), sb.f.get('AUTH')); assert.ok(sa.f.get('AUTH'));
  assert.deepEqual([sb.f.get('ADDR0'), sb.f.get('LADDR0'), sb.f.get('ADDR1')], ['10.77.0.2', '10.77.0.2', '10.77.0.3']);
  assert.equal(Number(sb.f.get('SYSFLAGS')) & (1 << 19), 1 << 19, 'started');
  // the game closes its lobby links at the start: the race (the game record) goes on
  a.close(); b.close();
  assert.equal(w.host.lobby.games.size, 1);
  assert.equal(w.host.lobby.byPersona.size, 0);
  // after the race the players log in again, with the same names
  const a2 = w.client('10.77.0.2').login('Alpha');
  assert.equal(a2.last('pers').code, '');
  a2.send('gsea', [['START', 0], ['COUNT', 20], ['CUSTFLAGS', 0], ['CUSTMASK', 0], ['SYSFLAGS', 0], ['SYSMASK', 0]]);
  assert.equal(a2.last('gsea').f.get('COUNT'), '0', 'a started race is not listed');
  w.advance(31 * 60_000);
  assert.equal(w.host.lobby.games.size, 0, 'forgotten after raceMs');
});

test('game passwords: advertised, required for joins by name or id, never published or logged', t => {
  const w = world(); t.after(() => w.host.stop());
  const a = w.client('10.77.0.2').login('Alpha'), b = w.client('10.77.0.3').login('Bravo');
  const password = 'Room Secret:%=42';
  a.send('gcre', [['NAME', 'Locked'], ['SYSFLAGS', 1 << 16], ['PASS', password]]);
  const id = Number(a.last('gcre').f.get('IDENT'));
  assert.equal(Number(a.last('gcre').f.get('SYSFLAGS')) & (1 << 16), 1 << 16);
  b.send('gsea');
  assert.equal(Number(b.last('+gam').f.get('SYSFLAGS')) & (1 << 16), 1 << 16);
  // A rejected join must leave the player's current game intact.
  b.send('gcre', [['NAME', 'Other']]);
  const other = w.host.lobby.gameByName('Other');
  for (const fields of [
    [['NAME', 'Locked']],
    [['IDENT', id], ['PASS', '']],
    [['NAME', 'Locked'], ['PASS', 'wrong']],
    [['IDENT', id], ['PASS', password.toLowerCase()]],
  ]) {
    w.advance(1000);
    b.send('gjoi', fields);
    assert.equal(b.last('gjoi').code, 'pass');
    assert.equal(w.host.lobby.gameByName('Locked').players.length, 1);
    assert.equal(w.host.lobby.games.get(other.id), other);
    assert.equal([...w.host.lobby.sessions].find(s => s.persona === 'Bravo').game, other.id);
  }
  // Neither the host's nor another player's gset can remove the requirement.
  a.send('gset', [['SYSFLAGS', 0], ['PASS', '']]);
  w.advance(1000);
  b.send('gjoi', [['IDENT', id], ['PASS', password]]);
  assert.equal(b.last('gjoi').code, '');
  assert.equal(b.last('gjoi').f.get('COUNT'), '2');
  assert.ok(!w.host.lobby.games.has(other.id));
  b.send('gset', [['SYSFLAGS', 0], ['PASS', '']]);
  const c = w.client('10.77.0.4').login('Charlie');
  c.send('gjoi', [['NAME', 'Locked']]);
  assert.equal(c.last('gjoi').code, 'pass');
  c.send('gjoi', [['NAME', 'Locked'], ['PASS', password]]);
  assert.equal(c.last('gjoi').code, '');
  // Repeating a join must not duplicate a player or delete the creator's game.
  a.send('gjoi', [['NAME', 'Locked'], ['PASS', password]]);
  b.send('gjoi', [['IDENT', id], ['PASS', password]]);
  const g = w.host.lobby.games.get(id);
  assert.equal(g.players.length, 3);
  assert.equal(Number(g.sysFlags) & (1 << 16), 1 << 16);
  assert.ok(!JSON.stringify(g, (k, v) => k === 'players' ? undefined : v).includes(password));
  assert.ok(!w.lines.join('\n').includes(password));
  for (const client of [a, b, c]) for (const m of client.msgs) {
    assert.equal(m.f.has('PASS'), false);
    assert.equal(m.f.has('PASSWORD'), false);
    assert.ok(!m.raw.includes(Buffer.from(password, 'latin1')));
  }
});

test('game passwords: a supplied password sets the flag; a locked game cannot silently become open', t => {
  const w = world(); t.after(() => w.host.stop());
  const a = w.client('10.77.0.2').login('Alpha'), b = w.client('10.77.0.3').login('Bravo');
  a.send('gcre', [['NAME', 'Invalid'], ['SYSFLAGS', 1 << 16]]);
  assert.equal(a.last('gcre').code, 'pass');
  assert.equal(w.host.lobby.games.size, 0);
  a.send('gcre', [['NAME', 'Locked'], ['SYSFLAGS', 0], ['PASS', 'secret']]);
  assert.equal(Number(a.last('gcre').f.get('SYSFLAGS')) & (1 << 16), 1 << 16);
  b.send('gjoi', [['NAME', 'Locked']]);
  assert.equal(b.last('gjoi').code, 'pass');
  a.send('gdel');
  a.send('gcre', [['NAME', 'Open'], ['SYSFLAGS', 0], ['PASS', '']]);
  assert.equal(a.last('gcre').code, '');
  assert.equal(Number(a.last('gcre').f.get('SYSFLAGS')) & (1 << 16), 0);
  b.send('gjoi', [['NAME', 'Open']]);
  assert.equal(b.last('gjoi').code, '');
});

test('the game list: every open game but a private one, the Filter\'s games first', () => {
  const w = world(), [a, b, c, d, e] = ['Alpha', 'Bravo', 'Charlie', 'Delta', 'Echo'].map((n, i) => w.client(`10.77.0.${i + 2}`).login(n));
  // the game's default search: race type 0, Collision Detection and Race Breaker on, no car restriction, unranked,
  // not private (CUSTMASK bit 8)
  const search = [['START', 0], ['COUNT', 20], ['CUSTFLAGS', 0x40000f3], ['CUSTMASK', 0x40001f3], ['SYSFLAGS', 0], ['SYSMASK', 0xc0000]];
  e.send('gsea', search);
  a.send('gcre', [['NAME', 'no collision'], ['CUSTFLAGS', 0x40000f2], ['PARAMS', 'TRACK=4011']]);
  b.send('gcre', [['NAME', 'private'], ['CUSTFLAGS', 0x40001f3], ['PARAMS', 'TRACK=4011']]);
  c.send('gcre', [['NAME', 'as asked'], ['CUSTFLAGS', 0x40000f3], ['PARAMS', 'TRACK=4011']]);
  d.send('gcre', [['NAME', 'ranked sprint'], ['CUSTFLAGS', 0x80000f3], ['SYSFLAGS', 1 << 18], ['PARAMS', 'TRACK=4011']]);
  w.advance(1000);
  const names = kind => e.take().filter(m => m.kind === kind).map(m => m.f.get('NAME'));
  assert.deepEqual(names('+agm').sort(), ['as asked', 'no collision', 'ranked sprint'], 'a private game is not announced');
  e.send('gsea', search);
  assert.equal(e.last('gsea').f.get('COUNT'), '3');
  assert.deepEqual(names('+gam'), ['as asked', 'no collision', 'ranked sprint']);
  e.send('gsea', [['START', 1], ['COUNT', 1], ...search.slice(2)]);
  assert.deepEqual(names('+gam'), ['no collision']);
  // a private game's players come by invitation: by its name
  e.send('gjoi', [['NAME', 'private']]);
  assert.equal(e.last('gjoi').code, '');
});

test('six players: a game asked for 8 takes 6, the seventh finds it full; +ses names all six', () => {
  const w = world(), names = ['Alpha', 'Bravo', 'Charlie', 'Delta', 'Echo', 'Foxtrot', 'Golf'];
  const cs = names.map((n, i) => w.client(`10.77.0.${i + 2}`).login(n));
  cs[0].send('gcre', [['NAME', 'six'], ['MAXSIZE', 8], ['PARAMS', 'TRACK=4123']]);
  assert.equal(cs[0].last('gcre').f.get('MAXSIZE'), '6');
  for (const c of cs.slice(1, 6)) { c.send('gjoi', [['NAME', 'six']]); assert.equal(c.last('gjoi').code, ''); }
  cs[6].send('gjoi', [['NAME', 'six']]);
  assert.equal(cs[6].last('gjoi').code, 'full');
  cs[0].send('gsta', [['NAME', 'six']]);
  const s = cs[5].last('+ses');
  assert.equal(s.f.get('COUNT'), '6'); assert.equal(s.f.get('OPPO5'), 'Foxtrot'); assert.equal(s.f.get('ADDR5'), '10.77.0.7');
});

test('Start Race before every player is ready: the room counts down, the players still not ready are kicked, the race starts', () => {
  const READY = 1 << 27, COUNTDOWN = 1 << 12, RANKED = 1 << 18, ACTIVE = 1 << 19;
  const w = world(), [a, b, c, d] = ['Alpha', 'Bravo', 'Charlie', 'Delta'].map((n, i) => w.client(`10.77.0.${i + 2}`).login(n));
  a.send('gcre', [['NAME', 'g'], ['MAXSIZE', 4], ['MINSIZE', 2], ['PARAMS', 'TRACK=4011']]);
  for (const x of [b, c, d]) x.send('gjoi', [['NAME', 'g']]);
  b.send('gset', [['NAME', 'g'], ['USERFLAGS', READY]]);
  const sys = m => Number(m.f.get('SYSFLAGS'));
  // the creator's Start Race (SPEED2.EXE 0x58aa20): the game's SYSFLAGS with the countdown, the creator ready;
  // only the countdown bit is the creator's to set
  a.send('gset', [['NAME', 'g'], ['SYSFLAGS', sys(a.last('+mgm')) | COUNTDOWN | RANKED | ACTIVE], ['USERFLAGS', READY]]);
  assert.equal(sys(a.last('gset')), COUNTDOWN);
  for (const x of [b, c, d]) assert.equal(sys(x.last('+mgm')), COUNTDOWN, 'every game in the room counts down');
  assert.equal(a.last('+mgm').f.get('OPFLAG0'), String(READY));
  c.send('gset', [['NAME', 'g'], ['SYSFLAGS', 0]]);
  assert.equal(sys(c.last('gset')), COUNTDOWN, 'a joiner does not stop it');
  // a start that fails clears it (0x58ab40), and Start Race sets it again
  a.send('gset', [['NAME', 'g'], ['SYSFLAGS', 0], ['USERFLAGS', 0]]);
  assert.equal(sys(b.last('+mgm')), 0);
  a.send('gset', [['NAME', 'g'], ['SYSFLAGS', COUNTDOWN], ['USERFLAGS', READY]]);
  // the countdown's end (0x58a7e0): the players not ready in one KICK list, then the start
  a.send('gset', [['NAME', 'g'], ['KICK', 'Charlie,Delta']]);
  for (const [x, n] of [[c, 'Charlie'], [d, 'Delta']]) {
    // the game leaves the room on the game's record after its own without the game (SPEED2.EXE 0x58dab0)
    const [who, mgm] = x.msgs.slice(-2);
    assert.deepEqual([who.kind, who.f.get('G')], ['+who', '0'], `${n} is out of the game`);
    assert.equal(mgm.kind, '+mgm');
    const named = [...mgm.f].filter(([k]) => k.startsWith('OPPO')).map(([, v]) => v);
    assert.ok(named.includes('Bravo') && !named.includes(n), `${n}'s game record without ${n}: ${named}`);
  }
  assert.equal(b.last('+mgm').f.get('COUNT'), '2');
  a.send('gsta', [['NAME', 'g']]);
  assert.equal(a.last('gsta').code, '');
  const s = b.last('+ses');
  assert.equal(s.f.get('COUNT'), '2'); assert.deepEqual([s.f.get('OPPO0'), s.f.get('OPPO1')], ['Alpha', 'Bravo']);
  assert.ok(!c.last('+ses') && !d.last('+ses'));
});

test('leaving: a joiner leaves, the creator leaves, the creator deletes; a browser that goes', () => {
  const w = world(), a = w.client('10.77.0.2').login('Alpha'), b = w.client('10.77.0.3').login('Bravo');
  a.send('gcre', [['NAME', 'g1'], ['MAXSIZE', 2], ['PARAMS', 'TRACK=1']]);
  b.send('gjoi', [['NAME', 'g1']]);
  const c = w.client('10.77.0.4').login('Charlie');
  c.send('gjoi', [['NAME', 'g1']]);
  assert.equal(c.last('gjoi').code, 'full');
  b.send('glea');
  assert.equal(a.last('+mgm').f.get('COUNT'), '1');
  b.send('gjoi', [['NAME', 'g1']]);
  a.send('gdel');
  assert.equal(b.last('+mgm').f.get('IDENT'), '1'); assert.equal(b.last('+mgm').f.get('COUNT'), undefined, 'IDENT only: deleted');
  assert.equal(w.host.lobby.games.size, 0);
  a.send('gcre', [['NAME', 'g2'], ['PARAMS', 'TRACK=1']]);
  b.send('gjoi', [['NAME', 'g2']]);
  w.host.peerGone(ip('10.77.0.2'));                       // the creator's browser leaves the switch
  assert.equal(w.host.lobby.games.size, 0); assert.equal(b.last('+mgm').f.get('COUNT'), undefined);
  assert.equal(w.host.lobby.sessions.size, 2);
});

test('limits: message flood, sessions per browser, frame size, silence', () => {
  const w = world({ limits: { perVip: 2 } }), g = w.client('10.77.0.2');
  const a = g.login('Alpha');
  for (let i = 0; i < 60; i++) a.send('~png', [['TIME', 1]]);
  assert.ok(a.reset, 'more than 40 messages at once: dropped');
  g.login('Beta'); g.login('Gamma');
  const d = g.login('Delta');
  assert.ok(d.reset, 'a third session from one browser: refused');
  const w2 = world(), h = w2.client('10.77.0.5'), big = h.connect(PORTS.lobby);
  big.send('mesg', [['TEXT', 'x'.repeat(9000)]]);
  assert.ok(big.reset, 'a message over 8 KB: dropped');
  const odd = h.login('Oddball');
  h.w.host.frame(h.vip, lanFrame(4, h.vip, VHOST_VIP, odd.port, PORTS.lobby, DATA, odd.cid, Buffer.concat([Buffer.from('a\x01bc'), Buffer.from([0, 0, 0, 0, 0, 0, 0, 13, 0])])));
  assert.ok(odd.reset, 'a kind that is not four letters: dropped');
  const quiet = h.login('Quiet');
  w2.advance(60_000); w2.advance(61_000);
  assert.ok(quiet.reset, 'silent for 120 s: dropped');
  assert.ok(quiet.msgs.some(m => m.kind === '~png'), 'pinged first');
});

// a real race report (SPEED2.EXE, a 2-lap circuit here: index 1 first in 186.82 s, index 0 second in 187.67 s)
const RESU = 'AwIAAQ+uAAIBAQEVQAADJ0M60exCuGZnQflKqkIogLdAsZmaQU2tD0I5HwoAAAAAAAAAAAAAAAAAAAAAAAAAAAIBFUAAAOhDO6uGQreo9kH44idCKNlYQL3CkEFciThCPgV4AAAAAAAAAAAAAAAAAAAAAAAAAA==';
test('results: the race report counts once, is checked, and shows in the career and the rankings', () => {
  const w = world(), a = w.client('10.77.0.2').login('Alpha'), b = w.client('10.77.0.3').login('Bravo');
  a.send('gcre', [['NAME', 'r1'], ['PARAMS', 'TRACK=4014']]); b.send('gjoi', [['NAME', 'r1']]);
  a.send('gsta', [['NAME', 'r1']]);
  const auth = a.last('+ses').f.get('AUTH');
  a.close(); b.close();
  const report = (c, extra = []) => c.send('rank', [['AUTH', auth], ['REPT', 'x'], ['RESU', RESU], ['VENUE', 'NFSUG2'],
    ['NAME0', 'Alpha'], ['TEAM0', 'NFSUG2'], ['NAME1', 'Bravo'], ['TEAM1', 'NFSUG2'], ...extra]);
  // too early: 186 s of racing cannot have happened 10 s after the start
  w.advance(10_000);
  const b2 = w.client('10.77.0.3').login('Bravo');
  report(b2);
  assert.equal(b2.last('rank').f.get('RANK'), 'Unranked');
  assert.ok(w.lines.some(l => l.includes('not taken: a race of 10.0 s (a circuit race takes 20 s at least)')), w.lines.join('\n'));
  // 30 s after the start: a race that long, but not 186 s of it
  w.advance(20_000);
  report(b2);
  assert.ok(w.lines.some(l => l.includes('not taken: a time the race has not had')));
  assert.equal(w.host.lobby.results.board(0).length, 0);
  // after the race: Bravo's report alone decides nothing (a report not taken was not his one report); Alpha's,
  // from another browser with the same places, makes the result
  w.advance(200_000);
  const b3 = w.client('10.77.0.3').login('Bravo');
  report(b3);
  assert.ok(!w.lines.some(l => /"r1" \(circuit/.test(l)), 'one report alone');
  assert.equal(w.host.lobby.results.board(0).length, 0);
  const a2 = w.client('10.77.0.2').login('Alpha');
  report(a2);
  assert.ok(w.lines.some(l => /"r1" \(circuit, unranked\): 1 Bravo 3:06\.82, 2 Alpha 3:07\.67/.test(l)), w.lines.join('\n'));
  report(a2);
  assert.equal(w.host.lobby.results.board(0).reduce((n, r) => n + r.races, 0), 2, 'counted once');
  assert.equal(w.host.lobby.games.size, 0, 'both players reported: the race is over');
  // a report for no race of this server's
  a2.send('rank', [['AUTH', 'f00'], ['RESU', RESU], ['NAME0', 'Alpha'], ['NAME1', 'Bravo']]);
  assert.ok(w.lines.at(-1).includes('no race of this server'));
  // the career: unranked win for Bravo (circuit block: values 9..13 = rating, ranked W/L, unranked W/L)
  a2.send('user', [['PERS', 'Bravo']]);
  const stat = a2.last('user').f.get('STAT').split(',').slice(0, 38).map(x => parseInt(x, 16));
  assert.equal(stat.length, 38);
  assert.deepEqual(stat.slice(8, 13), [0, 0, 0, 1, 0]);
  // the reporter got its career again right after its report (the game asked at the login, before it)
  const pushed = a2.msgs.filter(m => m.kind === 'user' && m.f.get('PERS') === 'Alpha');
  assert.ok(pushed.length >= 1);
  a2.send('user', [['PERS', 'Alpha']]);
  assert.deepEqual(a2.last('user').f.get('STAT').split(',').slice(8, 13).map(x => parseInt(x, 16)), [0, 0, 0, 0, 1]);
  // the rankings: the circuit board, and one's own row
  a2.send('snap', [['INDEX', 4], ['CHAN', 6], ['START', 0], ['RANGE', 100]]);
  assert.equal(a2.last('snap').f.get('COUNT'), '2');
  const rows = a2.msgs.filter(m => m.kind === '+snp').map(m => [m.f.get('N'), m.f.get('R'), m.f.get('S')]);
  assert.deepEqual(rows, [['Bravo', '1', '0,1,0'], ['Alpha', '2', '0,0,1']]);
  a2.send('snap', [['INDEX', 4], ['CHAN', 12], ['RANGE', 1], ['FIND', '$']]);
  assert.equal(a2.last('snap').f.get('COUNT'), '1'); assert.equal(a2.last('+snp').f.get('N'), 'Alpha');
  a2.send('snap', [['INDEX', 4], ['CHAN', 7], ['START', 0], ['RANGE', 100]]);
  assert.equal(a2.last('snap').f.get('COUNT'), '0', 'no sprint races yet');
});

test('names: a name another browser holds comes with four digits (the same for one client); its own browser takes it back', () => {
  const w = world();
  const a = w.client('10.77.0.2').login('NAME');
  assert.equal(a.last('pers').f.get('PERS'), 'NAME');
  const b = w.client('10.77.0.3').login('NAME'), bn = b.last('pers').f.get('PERS');
  assert.equal(b.last('pers').code, ''); assert.match(bn, /^NAME\d{4}$/);
  assert.equal(b.last('+who').f.get('N'), bn, 'the lobby knows the player by it');
  assert.ok(w.lines.some(l => l.includes(`plays as "${bn}" ("NAME" is taken)`)), w.lines.join('\n'));
  const c = w.client('10.77.0.4').login('NAME'), cn = c.last('pers').f.get('PERS');
  assert.match(cn, /^NAME\d{4}$/); assert.notEqual(cn, bn);
  assert.deepEqual(new Set(w.host.lobby.byPersona.keys()), new Set(['name', bn.toLowerCase(), cn.toLowerCase()]));
  // the digits take the end of a long name: still a persona name (15 characters at most)
  w.client('10.77.0.5').login('ABCDEFGHIJKLMNO');
  assert.match(w.client('10.77.0.6').login('ABCDEFGHIJKLMNO').last('pers').f.get('PERS'), /^ABCDEFGHIJK\d{4}$/);
  // the same client logging in again: the same digits (the game asks for them now: the runtime took them)
  b.close();
  assert.equal(w.client('10.77.0.3').login('NAME').last('pers').f.get('PERS'), bn);
  b.close();
  assert.equal(w.client('10.77.0.3').login(bn).last('pers').f.get('PERS'), bn);
  // ... and asking for the name while its older session still plays under the digits (its game's new lobby link):
  // the same digits, from that session
  assert.equal(w.client('10.77.0.3').login('NAME').last('pers').f.get('PERS'), bn);
  assert.equal([...w.host.lobby.sessions].filter(s => s.persona === bn).length, 1, 'the old session went');
  assert.equal(w.host.lobby.byPersona.size, 5, 'NAME and two with digits, the long name and one with digits');
  // the holder's own browser logging in again (its game's new lobby link): the name, from its old session
  const a2 = w.client('10.77.0.2').login('NAME');
  assert.equal(a2.last('pers').f.get('PERS'), 'NAME');
  assert.equal([...w.host.lobby.sessions].filter(s => s.persona === 'NAME').length, 1, 'the old session went');
  // a shortened name that reads as a slur (names.mjs) is "Player" with the digits
  w.client('10.77.0.7').login('AbcdefghFagxyz');
  assert.match(w.client('10.77.0.8').login('AbcdefghFagxyz').last('pers').f.get('PERS'), /^Player\d{4}$/);
});

test('names: a racer keeps the name until back; another player under it gets digits and ends nothing', () => {
  const w = world(), a = w.client('10.77.0.2').login('Alpha'), b = w.client('10.77.0.3').login('Bravo');
  a.send('gcre', [['NAME', 'r2'], ['PARAMS', 'TRACK=4014']]); b.send('gjoi', [['NAME', 'r2']]);
  a.send('gsta', [['NAME', 'r2']]);
  const auth = a.last('+ses').f.get('AUTH');
  a.close(); b.close();                                  // the game closes its lobby link for the race
  // another browser under the racer's name: digits, and its game list does not take the racer back
  const other = w.client('10.77.0.4').login('Alpha');
  assert.match(other.last('pers').f.get('PERS'), /^Alpha\d{4}$/);
  other.send('gsea', [['START', 0], ['COUNT', 20]]);
  w.advance(200_000);
  const report = c => c.send('rank', [['AUTH', auth], ['REPT', 'x'], ['RESU', RESU], ['VENUE', 'NFSUG2'], ['NAME0', 'Alpha'], ['NAME1', 'Bravo']]);
  const b2 = w.client('10.77.0.3').login('Bravo');
  b2.send('gsea', [['START', 0], ['COUNT', 20]]); report(b2);
  assert.equal(w.host.lobby.games.size, 1, 'Alpha is still racing');
  // the racer, back from its own browser: its name, and its report makes the result
  const a2 = w.client('10.77.0.2').login('Alpha');
  assert.equal(a2.last('pers').f.get('PERS'), 'Alpha');
  report(a2);
  assert.ok(w.lines.some(l => /"r2" \(circuit, unranked\): 1 Bravo 3:06\.82, 2 Alpha 3:07\.67/.test(l)), w.lines.join('\n'));
  assert.equal(w.host.lobby.games.size, 0);
});

test('web pages: the news and terms pages as web-offer scripts, nothing else', () => {
  const w = world(), g = w.client('10.77.0.2');
  const get = (req) => {
    const c = g.connect(PORTS.http), out = [];
    g.conns.get(c.port).r = { push: b => { out.push(Buffer.from(b)); return []; } };
    w.host.frame(g.vip, lanFrame(4, g.vip, VHOST_VIP, c.port, PORTS.http, DATA, c.cid, Buffer.from(req)));
    return { text: Buffer.concat(out).toString('latin1'), fin: c.fin };
  };
  const n = get('GET /news HTTP/1.0\r\nUser-Agent: Custom/1.0\r\n\r\n');
  assert.match(n.text, /^HTTP\/1\.0 200 OK\r\n/); assert.match(n.text, /\r\n\r\n%\{ CMD=news TITLE="NFSU2 Online" BTN1="OK" BTN1-GOTO="\$quit" %\}\r\n/);
  assert.ok(n.fin);
  assert.match(get('GET /tos HTTP/1.0\r\n\r\n').text, /Terms of Service/);
  assert.match(get('GET /card HTTP/1.0\r\n\r\n').text, /^HTTP\/1\.0 404/);
  assert.match(get('POST /news HTTP/1.0\r\n\r\n').text, /^HTTP\/1\.0 400/);
  for (const p of ['/news', '/tos']) assert.ok(!/CMD=(card|prom)/.test(get(`GET ${p} HTTP/1.0\r\n\r\n`).text));
});

test('EA Messenger: the login by LKEY, friends, presence, messages, blocking, leaving', async () => {
  const w = world(), ga = w.client('10.77.0.2'), gb = w.client('10.77.0.3');
  const la = ga.login('Alpha'), lb = gb.login('Bravo');
  const key = c => c.last('pers').f.get('LKEY');
  const buddy = (g, k) => {
    const c = g.connect(PORTS.buddy);
    c.send('AUTH', [['PROD', 'NFS-CONSOLE-2005'], ['VERS', '0.1'], ['PRES', 20920], ['USER', '/PC/NFS-CONSOLE-2005'], ['LKEY', k]]);
    return c;
  };
  const a = buddy(ga, key(la)), b = buddy(gb, key(lb));
  assert.equal(a.last('AUTH').code, ''); assert.equal(a.last('AUTH').f.get('TITL'), 'EA MESSENGER');
  a.send('EPGT', [['LRSC', 'PC'], ['ID', 4]]);
  assert.equal(a.last('EPGT').f.get('ENAB'), 'N');
  a.send('RGET', [['LRSC', 'PC'], ['LIST', 'B'], ['PRES', 'Y'], ['PEND', 'Y'], ['ID', 1]]);
  assert.deepEqual([a.last('RGET').f.get('ID'), a.last('RGET').f.get('SIZE')], ['1', '0']);
  // the Search finds Bravo, not Alpha itself
  a.send('USCH', [['USER', 'bra'], ['MAXR', 10], ['ID', 3]]);
  assert.equal(a.last('USCH').f.get('SIZE'), '1'); assert.equal(a.last('USER').f.get('USER'), 'Bravo');
  a.send('USCH', [['USER', 'alp'], ['ID', 3]]);
  assert.equal(a.last('USCH').f.get('SIZE'), '0');
  // Alpha adds Bravo: Bravo hears of it, Alpha sees Bravo's presence
  b.send('PSET', [['SHOW', 'CHAT'], ['STAT', 'EX=0\nP=nfs5\n'], ['PROD', 'is playing Underground 2']]);
  a.send('RADD', [['LRSC', 'PC'], ['USER', 'Bravo'], ['LIST', 'B'], ['ID', 5]]);
  assert.equal(a.last('RADD').code, '');
  assert.equal(a.last('PGET').f.get('USER'), 'Bravo'); assert.equal(a.last('PGET').f.get('SHOW'), 'CHAT');
  assert.equal(b.last('RNOT').f.get('USER'), 'Alpha');
  a.send('RGET', [['LIST', 'B'], ['ID', 2]]);
  assert.equal(a.last('RGET').f.get('SIZE'), '1'); assert.equal(a.last('ROST').f.get('USER'), 'Bravo');
  // presence changes reach the friend (at most once a second)
  w.advance(1000);
  b.send('PSET', [['SHOW', 'AWAY'], ['STAT', ''], ['PROD', 'racing']]);
  assert.equal(a.last('PGET').f.get('SHOW'), 'AWAY');
  // a message, and a game invite passed on with the sender named by the server
  a.send('SEND', [['USER', 'Bravo'], ['BODY', 'race?\x07'], ['SUBJ', ''], ['TYPE', 'C']]);
  assert.equal(b.last('RECV').f.get('USER'), 'Alpha'); assert.equal(b.last('RECV').f.get('BODY'), 'race?');
  a.send('GINV', [['USER', 'Bravo'], ['HOST', 'Alpha'], ['TYPE', 'G'], ['LKEY', 'x']]);
  assert.equal(b.last('GINV').f.get('USER'), 'Alpha'); assert.equal(b.last('GINV').f.get('HOST'), 'Alpha');
  assert.equal(b.last('GINV').f.get('LKEY'), undefined);
  // a friend request from Bravo, accepted by Alpha: both lists
  b.send('RADM', [['LRSC', 'PC'], ['USER', 'Alpha'], ['ID', 102], ['PRES', 'Y']]);
  assert.equal(a.last('RNOT').f.get('USER'), 'Bravo'); assert.equal(a.last('RNOT').f.get('ATTR'), String(0x80000));
  a.send('RRSP', [['LRSC', 'PC'], ['USER', 'Bravo'], ['ANSW', 'Y'], ['ID', 7]]);
  assert.equal(b.last('RNOT').f.get('USER'), 'Alpha'); assert.equal(b.last('RNOT').f.get('CHNG'), 'A');
  assert.ok(w.host.buddies.list('Bravo').B.has('alpha'));
  const friendsLines = () => w.lines.filter(l => l.includes('are friends')).length;
  assert.equal(friendsLines(), 1);
  a.send('RRSP', [['USER', 'Bravo'], ['ANSW', 'Y']]);
  assert.equal(friendsLines(), 1, 'an answer without a request does nothing');
  // crossing requests make friends too (the asked one uses "Send Friend Request" back)
  const gc = w.client('10.77.0.4'), lc = gc.login('Charlie'), c = buddy(gc, key(lc));
  c.send('RADM', [['USER', 'Alpha'], ['ID', 1]]);
  assert.equal(a.last('RNOT').f.get('USER'), 'Charlie'); assert.equal(a.last('PGET').f.get('USER'), 'Charlie');
  a.send('RADM', [['USER', 'Charlie'], ['ID', 2]]);
  assert.equal(friendsLines(), 2);
  assert.equal(c.last('RNOT').f.get('ATTR'), '0');
  assert.ok(w.host.buddies.list('Charlie').B.has('alpha') && w.host.buddies.list('Alpha').B.has('charlie'));
  // Bravo blocks Alpha: Alpha's messages no longer arrive
  b.send('RADD', [['USER', 'Alpha'], ['LIST', 'I'], ['ID', 9]]);
  const before = b.msgs.filter(m => m.kind === 'RECV').length;
  a.send('SEND', [['USER', 'Bravo'], ['BODY', 'hello?']]);
  assert.equal(b.msgs.filter(m => m.kind === 'RECV').length, before);
  // Bravo leaves: Alpha sees him offline; the friends list stays
  b.send('DISC');
  assert.equal(a.last('PGET').f.get('SHOW'), 'DISC');
  assert.ok(w.host.buddies.list('Alpha').B.has('bravo'));
  // an unknown key: refused once the lobby had a moment to finish
  const x = buddy(w.client('10.77.0.9'), '0123456789abcdef0123456789');
  await new Promise(r => setTimeout(r, 3300));
  assert.equal(x.last('AUTH').code, 'pass');
  x.send('RGET', [['LIST', 'B']]);
  assert.equal(x.last('RGET').code, 'auth');
});

test('the count of players in Online mode (the server\'s /players): logged in, or racing until back', () => {
  const w = world(), a = w.client('10.77.0.2').login('Alpha'), b = w.client('10.77.0.3').login('Bravo');
  // and the browsers it counts (vips(): the server's console), the same
  const count = () => { const i = w.host.info(), v = [...w.host.vips()].sort(); assert.equal(v.length, i.personas + i.racing); return [i.personas, i.racing]; };
  assert.deepEqual(count(), [2, 0]);
  a.send('gcre', [['NAME', 'c1'], ['PARAMS', 'TRACK=4014']]); b.send('gjoi', [['NAME', 'c1']]);
  a.send('gsta', [['NAME', 'c1']]);
  a.close(); b.close();                                  // the game closes its lobby link for the race
  assert.deepEqual(count(), [0, 2], 'racing: still in Online mode');
  w.host.peerGone(ip('10.77.0.3'));                      // Bravo's browser leaves during the race
  assert.deepEqual(count(), [0, 1]);
  assert.deepEqual([...w.host.vips()], [ip('10.77.0.2')], 'Alpha, racing');
  w.advance(200_000);
  w.client('10.77.0.2').login('Alpha');                  // back online under the name: counted once
  assert.deepEqual(count(), [1, 0]);
});
