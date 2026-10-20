// The lobby of the online host (online/index.mjs): what EA's servers did for
// NFS Underground 2's Online mode, for the players of this one server.
//
// Message shapes: those of EA's own lobby code, recorded from server.dll (the
// LAN server, which runs inside a hosting browser) with the dev trace
// (online/trace.mjs), plus the online-only parts (the config blob, rooms,
// personas, the buddy login) from the game's code and what the game itself
// was seen to do.
//
// One session per lobby connection:
//   connect  -> the game sends addr, skey, news NAME=7 (asks for the config)
//   sele     -> what the session wants pushed
//   auth     -> any name logs in but one that reads as a slur (names.mjs; no accounts, nothing stored); PERSONAS
//   acct     -> "create account": logs in the same way
//   cper/dper-> a persona for this session, or one less (at most LIMITS.personas)
//   pers     -> the persona to play as; LKEY.  One holder per name while online
//               or racing (the holder's own browser takes it back): a name
//               another browser holds is given with four digits in place of
//               its end, and the game takes it for its own (runtime/online.c)
//   move     -> into a room (+who, +usr, +pop), or out (an unknown NAME)
//   mesg     -> chat in the room, or to one persona (PRIV; the game's
//               challenge invites are private messages too): +msg
//   gsea/gcre/gjoi/gset/gsta/gdel/glea -> the game list and one game: +gam,
//               +mgm, +ses (the race starts: every player's virtual address)
//   ~png     -> the server pings every pingMs and drops a session that says
//               nothing for idleMs
//   user/onln-> a persona's record (user: the online career, STAT, from stats.mjs)
//   rank     -> a player's report of a finished race (once per player and race,
//               only for a race this server started, checked): the results
//   snap     -> a ranking (the overall boards per race type) or one row of it
// At the race start the game closes its lobby link (as with server.dll in
// LAN play) and opens a new one afterwards (it logs in again and reports the
// result: rank).  A started game therefore outlives its players' sessions,
// for raceMs at most.
//
// Never read on login: PASS (the account password).  REGKEY (the CD key) and
// PSES (a checksum of the player's files) are never read on any command.  Never relayed: the
// game's MID (its adapter address).  Every address in a reply is the
// browser's virtual one, which the switch wrote into the frame.
//
// What one client can make the lobby do to the others (the Online audit,
// ONL-2, ONL-5, ONL-7, ONL-9): sessions are counted per browser, per client
// (its opaque group id from the switch) and per site, and the last freeShare
// of them are only for clients that hold none; a game's record goes to the
// players looking at the game list (since their last gsea, until they are in
// a game), at most once a second per game, the latest; a room's population at
// most once a second; a game's settings only when they changed; games
// created, joined and car descriptions published have their own rates; a
// browser keeps openPerVip open games and startedPerVip started ones (a
// client openPerGroup, startedPerGroup); a race needs players in different
// browsers, and is over once its players' browsers have been gone from the
// switch for orphanMs.  A race's result counts once two of its players, in
// different browsers, reported the same places, no sooner than the race
// type's shortest race; a ranked race needs three players for its points.
import crypto from 'node:crypto';
import { FrameReader, encodeFrame, encodeFields, fieldMap, MAX_FRAME, SECRET_KEYS } from './aries.mjs';
import { Stats, RACE_TYPES, decodeResult } from './stats.mjs';
import { blockedName } from '../names.mjs';

export const LOBBY_LIMITS = Object.freeze({
  sessions: 1000,           // lobby sessions at once
  perVip: 3,                // lobby sessions from one browser (a reconnect overlaps the old one briefly)
  perGroup: 8,              // from one client (one address: a household's browsers)
  perSite: 32,              // from one site (an IPv6 /48)
  freeShare: 0.25,          // the last quarter of the sessions: only for a client that holds none
  msgRate: 20,              // messages a second from one session (burst: twice that)
  chatRate: 1,              // chat messages a second (burst 5); over it: muted for muteMs
  chatLen: 255,             // characters of one chat message
  muteMs: 10_000,
  createRate: 0.5,          // games created a second by one session (burst 3)
  actRate: 1,               // games joined, rooms entered and car descriptions published a second (burst 4)
  announceMs: 1000,         // a game's record to the game list, a room's population: at most this often
  personas: 4,              // per session (the game shows four)
  games: 300,               // games at once
  openPerVip: 2,            // open games (not started) with a creator in one browser
  openPerGroup: 4,          // in one client
  startedPerVip: 2,         // started games (until they end) with a creator in one browser
  startedPerGroup: 8,       // in one client
  gameSize: 6,              // players in one game (the race coordinator's racer entries; runtime/netrace.c)
  gameNameLen: 32,
  auxiLen: 2048,            // the car description a player publishes (auxi TEXT)
  pingMs: 20_000,           // a ~png this often
  idleMs: 120_000,          // nothing from the game for this long: the session goes
  preLoginMs: 120_000,      // connected but no persona chosen for this long: goes
  raceMs: 30 * 60_000,      // a started game is forgotten this long after the start
  orphanMs: 120_000,        // a started game whose players' browsers left the switch this long ago ends
});

// the rooms everyone shares: a lounge and one per race type
export const ROOMS = Object.freeze(['Lounge', 'Circuit', 'Sprint', 'Drift', 'Drag', 'StreetX', 'URL'].map((name, i) => ({ id: i + 1, name })));
const ROOM_LIMIT = 50;

// the server config (the "news" reply, code new7) the game asks for at connect: where the buddy server is
// (here: this same host), the news and terms pages, the tier points per race type
const TIER = '0,1999,4999,9999,19999,39999,59999,79999,99999,119999';
export function configFields(ports) {
  return [
    ['TOSURL', 'http://10.77.0.1/tos'], ['NEWSURL', 'http://10.77.0.1/news'],
    ['BUDDY_SERVER', '10.77.0.1'], ['BUDDY_PORT', ports.buddy],
    ['CIRCUIT_TIER_POINTS', TIER], ['SPRINT_TIER_POINTS', TIER], ['DRIFT_TIER_POINTS', TIER], ['DRAG_TIER_POINTS', TIER],
    ['STREET_CROSS_TIER_POINTS', TIER], ['URL_TIER_POINTS', TIER],
  ];
}

// OPFLAG bit of a player who is ready (gset USERFLAGS, server.dll), SYSFLAGS bits of a game with a password
// and of a started one
const READY = 1 << 27, PASSWORD = 1 << 16, ACTIVE = 1 << 19;
// SYSFLAGS bit of a ranked game (Options: Game Type Ranked; the game then asks for 3 players at least, and its
// lobby's default search asks for unranked games: SYSMASK bits 18 and 19)
const RANKED = 1 << 18;
// SYSFLAGS bit of a game counting down to its start (the creator's gset; server.dll takes the creator's SYSFLAGS):
// Start Race while some players are ready and others not sets it (SPEED2.EXE 0x58aa20), every game in the room then
// counts down 10 s (0x4d2852, 0x4e5982), and the creator's game kicks the players still not ready (gset KICK, a list)
// and starts (0x58a7e0); a start that fails clears it (0x58ab40)
const COUNTDOWN = 1 << 12;
// Only a randomly keyed digest of a room password lives with its game, never in a reply or on disk.
const roomPassword = (key, value) => crypto.createHmac('sha256', key).update(value, 'latin1').digest();
// CUSTFLAGS bit of a private game (Options: Private Room, SPEED2.EXE 0x58d55d; the game's own search always asks for
// it clear, 0x590669: its players come by invitation)
const PRIVATE = 1 << 8;
// the race types a report may name: RACE_TYPES, which have their stat blocks, and the game's seventh (a game with
// CUSTFLAGS bit 25, which SPEED2.EXE 0x5fe8d0 reports as 6), which has no stat block or tier points: its results
// count nowhere
const REPORT_TYPES = Object.freeze([...RACE_TYPES, 'type 6']);
// the shortest race of each type (REPORT_TYPES order) a result may come from, in seconds: well under the game's
// own shortest (one lap of the shortest track; a quarter-mile drag)
const MIN_RACE_S = Object.freeze([20, 20, 5, 20, 20, 20, 20]);

export class Bucket {
  constructor(rate, burst, now) { this.rate = rate; this.cap = burst; this.v = burst; this.now = now; this.t = now(); }
  take(n = 1) {
    const t = this.now();
    this.v = Math.min(this.cap, this.v + (t - this.t) / 1000 * this.rate); this.t = t;
    if (this.v < n) return false;
    this.v -= n; return true;
  }
}

/** a persona or account name: what the game lets one type (letters, digits, - and _), 3 to 15 characters */
export function validName(s) {
  return typeof s === 'string' && /^[A-Za-z0-9_-]{3,15}$/.test(s);
}
/** text another player will see (chat, game names): printable, no control or format characters */
export function cleanText(s, max) {
  return [...String(s ?? '').normalize('NFC').replace(/[\p{Cc}\p{Cf}]/gu, ' ').replace(/\s+/g, ' ').trim()].slice(0, max).join('');
}
const fmtTime = t => `${Math.floor(t / 60)}:${(t % 60).toFixed(2).padStart(5, '0')}`;
const day = (t) => { const d = new Date(t); return `${d.getUTCFullYear()}.${d.getUTCMonth() + 1}.${d.getUTCDate()}`; };
/** fields within max bytes of text: the optional ones (named in drop, first dropped first) go until it fits */
function fit(fields, max, drop) {
  let f = fields;
  for (const k of [null, ...drop]) {
    if (k) f = f.filter(([n]) => n !== k);
    if (encodeFields(f).length <= max) return f;
  }
  return f;
}

export class Lobby {
  constructor({ log = s => console.log(s), limits = {}, now = () => Date.now(), ports, stats = null } = {}) {
    this.L = { ...LOBBY_LIMITS };
    for (const k of Object.keys(LOBBY_LIMITS)) if (Number.isFinite(limits?.[k]) && limits[k] > 0) this.L[k] = limits[k];
    this.log = log; this.now = now; this.ports = ports;
    this.results = stats ?? new Stats({ now, log });
    this.sessions = new Set();
    this.byPersona = new Map();          // lower-case persona -> session (who holds the name now)
    this.rooms = new Map(ROOMS.map(r => [r.id, { ...r, users: new Set() }]));
    this.games = new Map();              // id -> game
    this.agm = new Map();                // game id -> { at, timer, fn, gone }: its record to the game list, at most every announceMs
    this.pop = new Map();                // room id -> the same for its population
    this.goneVips = new Map();           // a browser's address -> when it left the switch (not back since)
    this.nextUser = 1; this.nextGame = 1;
    this.stats = { logins: 0, refused: 0, chats: 0, games: 0, races: 0, dropped: 0 };
    this.timer = setInterval(() => this.tick(), 5000);
    this.timer.unref?.();
  }
  /** here(vip): that browser is still on the switch.  racing: players of a started race not back in the lobby
   *  yet (their lobby link is closed while they race) and not online under their name again */
  info(here = () => true) {
    return { sessions: this.sessions.size, personas: this.byPersona.size, racing: this.racers(here).length, games: this.games.size, ...this.stats };
  }
  /** the browsers in Online mode, as info() counts them: a persona in the lobby, or racing (the server's console) */
  vips(here = () => true) {
    return new Set([...[...this.byPersona.values()].map(s => s.vip), ...this.racers(here).map(p => p.vip)]);
  }
  racers(here) {
    const out = [];
    for (const g of this.games.values()) if (g.started) for (const p of g.players)
      if (!p.back && !this.byPersona.has(p.name.toLowerCase()) && here(p.vip)) out.push(p);
    return out;
  }
  stop() {
    clearInterval(this.timer);
    for (const a of [...this.agm.values(), ...this.pop.values()]) clearTimeout(a.timer);
    for (const s of [...this.sessions]) s.conn.destroy('server stopping');
    this.results.stop();
  }
  /** a frame from a browser: it is on the switch */
  seen(vip) { if (this.goneVips.size) this.goneVips.delete(vip); }
  /** a browser left the switch */
  peerGone(vip) { this.goneVips.set(vip, this.now()); }
  /** fn now, or (only the latest fn) once ms have passed since the last one: a = { at, timer, fn } */
  later(a, ms, fn) {
    a.fn = fn;
    if (a.timer) return;
    const wait = a.at + ms - this.now();
    if (wait <= 0) return this.fire(a);
    a.timer = setTimeout(() => this.fire(a), wait);
    a.timer.unref?.();
  }
  fire(a) { clearTimeout(a.timer); a.timer = null; a.at = this.now(); const fn = a.fn; a.fn = null; fn?.(); }

  // ---- the redirector (20920): @dir -> this lobby, and nothing else ----
  redirector(conn) {
    const r = new FrameReader();
    const t = setTimeout(() => conn.destroy('redirector: no request'), 30_000);
    conn.onclose = () => clearTimeout(t);
    conn.onend = () => conn.end();
    conn.ondata = buf => {
      for (const m of r.push(buf)) {
        // @tic asks for an RC4 ticket: no answer, so the lobby link stays plain (an 84-byte answer would key RC4)
        if (m.kind !== '@dir') continue;
        conn.write(encodeFrame('@dir', 0, [['ADDR', '10.77.0.1'], ['PORT', this.ports.lobby],
          ['SESS', crypto.randomInt(1, 2 ** 31)], ['MASK', crypto.randomBytes(16).toString('hex')]]));
        clearTimeout(t);
        conn.end();
      }
      if (r.error) conn.destroy('redirector: not the lobby protocol');
    };
  }

  // ---- the lobby (20922) ----
  connect(conn) {
    const L = this.L;
    let vip = 0, group = 0, site = 0;
    for (const s of this.sessions) {
      if (s.vip === conn.vip) vip++;
      if (s.group === conn.group) group++;
      if (conn.site != null && s.site === conn.site) site++;
    }
    const lastShare = this.sessions.size >= L.sessions * (1 - L.freeShare);
    if (this.sessions.size >= L.sessions || vip >= L.perVip || group >= L.perGroup || site >= L.perSite || (lastShare && (group || site))) {
      this.stats.refused++;
      this.log(`[online] ${conn.peer}: lobby full or too many sessions from that browser or client, refused`, conn.group);
      return conn.destroy('lobby full');
    }
    const t = this.now();
    const s = {
      conn, vip: conn.vip, group: conn.group, site: conn.site ?? null, addr: conn.peer, reader: new FrameReader(MAX_FRAME), id: this.nextUser++,
      account: null, personas: [], persona: null, lkey: null, room: null, game: null, auxi: '', browsing: false,
      t0: t, lastIn: t, lastPing: t, msgs: new Bucket(L.msgRate, 2 * L.msgRate, this.now),
      chats: new Bucket(L.chatRate, 5, this.now), creates: new Bucket(L.createRate, 3, this.now), acts: new Bucket(L.actRate, 4, this.now),
      muteUntil: 0, gone: false,
    };
    this.sessions.add(s);
    this.goneVips.delete(conn.vip);
    conn.ondata = buf => this.data(s, buf);
    conn.onend = () => { this.leave(s, 'closed by the game'); conn.end(); };
    conn.onclose = why => this.leave(s, why);
  }
  /** a session's words to the game */
  send(s, kind, fields = [], code = 0) { if (!s.gone) s.conn.write(encodeFrame(kind, code, fields)); }

  data(s, buf) {
    s.lastIn = this.now();
    for (const m of s.reader.push(buf)) {
      if (s.gone) return;
      if (!s.msgs.take()) { this.drop(s, 'too many messages'); return; }
      if (!/^[!-~]{4}$/.test(m.kind)) { this.drop(s, 'not the lobby protocol (a message kind that is not 4 letters)'); return; }
      // SPEED2.EXE 0x58d5d0 (create) and 0x58d7bb (join) send the room password as PASS.
      // Decode it only for these commands; account passwords still never reach a handler.
      const roomPasswordMessage = m.kind === 'gcre' || m.kind === 'gjoi';
      const f = fieldMap(m.body, key => SECRET_KEYS.includes(key) && !(roomPasswordMessage && key === 'PASS'));
      try { this.message(s, m.kind, f); } catch (e) {
        console.error(`[online] ${this.who(s)}: "${m.kind}" failed: ${e?.stack || e}`);
        this.drop(s, 'server error');
      }
    }
    if (s.reader.error) this.drop(s, `not the lobby protocol (${s.reader.error})`);
  }
  who(s) { return `${s.addr}${s.persona ? ` "${s.persona}"` : s.account ? ` (${s.account})` : ''}`; }
  drop(s, why) { this.stats.dropped++; this.log(`[online] ${this.who(s)}: dropped: ${why}`, s.group); s.conn.destroy(why); }

  message(s, kind, f) {
    switch (kind) {
      // ---- before the login ----
      case 'addr': return;                                  // the game's own idea of its address (server.dll: no answer)
      case 'skey': return this.send(s, 'skey', [['SKEY', '$' + crypto.randomBytes(16).toString('hex')]]);
      case 'news': return f.get('NAME') === '7' ? this.send(s, 'news', configFields(this.ports), 'new7') : this.send(s, 'news', [], 'badc');
      case 'sele': return this.send(s, 'sele', [['GAMES', 1], ['MYGAME', 1], ['ROOMS', 1], ['USERS', 1], ['USERSETS', 0], ['MESGS', 1],
        ['ASYNC', 1], ['RANKS', 0], ['STATS', Math.min(5000, Number(f.get('STATS') ?? 0) | 0)], ['MORE', 1], ['SLOTS', 36]]);
      case '~png': case '@alv': case '@cnt': return;       // keep-alive answers (lastIn is enough)
      case 'auth': case 'acct': return this.auth(s, kind, f);
      case 'lost': return this.send(s, 'lost', [], 'imst');  // nothing to recover: there are no accounts
      // ---- personas ----
      case 'cper': return this.createPersona(s, f);
      case 'dper': return this.deletePersona(s, f);
      case 'pers': return this.selectPersona(s, f);
    }
    if (!s.persona) return this.send(s, kind, [], 'auth');  // everything else needs a persona
    switch (kind) {
      case 'move': return this.move(s, f);
      case 'mesg': return this.mesg(s, f);
      case 'auxi': return this.auxi(s, f);
      case 'user': return this.userInfo(s, f);
      case 'onln': return this.online(s, f);
      // the game list: this player looks at it now (and gets its changes) until it is in a game
      case 'gsea': s.browsing = true; this.backFromRace(s); return this.gameSearch(s, f);
      case 'gcre': return this.gameCreate(s, f);
      case 'gjoi': return this.gameJoin(s, f);
      case 'gset': return this.gameSet(s, f);
      case 'gsta': return this.gameStart(s, f);
      case 'gdel': return this.gameDelete(s, f);
      case 'glea': return this.gameLeave(s, 'glea');
      case 'rank': return this.rank(s, f);
      case 'snap': return this.snap(s, f);
      case 'rept':
        this.log(`[online] ${this.who(s)}: a complaint about "${cleanText(f.get('PERS'), 16)}"`, s.group);
        return this.send(s, 'rept', [['TEXT', 'Report complete']]);
      default: return this.send(s, kind, [], 'badc');
    }
  }

  // ---- login ----
  auth(s, kind, f) {
    const name = f.get('NAME') ?? '';
    if (s.account) return this.send(s, kind, [], 'logn');
    // a name that reads as a slur (names.mjs) as one the game would not let one type
    if (!validName(name) || blockedName(name)) return this.send(s, kind, [], kind === 'acct' ? 'inam' : 'imst');
    s.account = name;
    // the account's personas: the name itself (cper adds more for this session)
    s.personas = [name];
    this.stats.logins++;
    this.log(`[online] ${s.addr}: logged in as "${name}" (${this.sessions.size} connected)`, s.group);
    const t = day(this.now());
    // the game keeps the reply's text in 122 bytes: the optional fields go first
    this.send(s, kind, fit([['NAME', name], ['PERSONAS', s.personas.join(',')], ['TOS', 1], ['ADDR', s.addr], ['LAST', t], ['BORN', '19800101'],
      ['SPAM', 'NN'], ['MAIL', 'player@localhost']], 122, ['MAIL', 'SPAM', 'BORN', 'LAST']));
    for (const r of this.rooms.values()) this.send(s, '+rom', this.roomFields(r));
  }
  createPersona(s, f) {
    if (!s.account) return this.send(s, 'cper', [], 'auth');
    const p = f.get('PERS') ?? '';
    if (!validName(p) || blockedName(p)) return this.send(s, 'cper', [], 'invp');
    if (s.personas.some(x => x.toLowerCase() === p.toLowerCase()) || this.byPersona.has(p.toLowerCase())) return this.send(s, 'cper', [], 'dupl');
    if (s.personas.length >= this.L.personas) return this.send(s, 'cper', [], 'nspc');
    s.personas.push(p);
    this.send(s, 'cper', this.persFields(s, p));
  }
  deletePersona(s, f) {
    if (!s.account) return this.send(s, 'dper', [], 'auth');
    const p = f.get('PERS') ?? '', i = s.personas.findIndex(x => x.toLowerCase() === p.toLowerCase());
    if (i < 0 || (s.persona && s.persona.toLowerCase() === p.toLowerCase())) return this.send(s, 'dper', [['PERS', p], ['RESULT', 1]]);
    s.personas.splice(i, 1);
    this.send(s, 'dper', [['PERS', p], ['RESULT', 0]]);
  }
  /** the pers / cper reply (the game keeps it in 108 bytes) */
  persFields(s, p) {
    const t = day(this.now());
    return fit([['PERS', p], ['LKEY', s.lkey ?? ''], ['NAME', s.account], ['LAST', t], ['PLAST', t]], 108, ['PLAST', 'LAST', 'NAME']);
  }
  selectPersona(s, f) {
    if (!s.account) return this.send(s, 'pers', [], 'auth');
    const p = f.get('PERS') ?? '';
    let mine = s.personas.find(x => x.toLowerCase() === p.toLowerCase());
    if (!mine) return this.send(s, 'pers', [], 'invp');
    this.giveWay(mine, s);
    // one holder per name, online or racing: a name another player holds is this player's with digits.  The answer
    // names it, and the runtime puts it in the game's own persona and account fields (runtime/online.c): the game
    // finds itself among a race's players by them (one that kept the name it asked for raced against itself)
    const asked = mine;
    if (this.nameTaken(mine, s) && !(mine = this.freeVariant(mine, s))) return this.send(s, 'pers', [], 'pset');
    if (s.persona && s.persona !== mine) this.byPersona.delete(s.persona.toLowerCase());
    // the key of the buddy login: the game keeps 31 characters of it (its buddy AUTH carried 31 of 32)
    s.persona = mine; s.lkey = crypto.randomBytes(15).toString('hex');
    this.byPersona.set(mine.toLowerCase(), s);
    this.log(`[online] ${s.addr}: plays as "${mine}"${mine !== asked ? ` ("${asked}" is taken)` : ''}`, s.group);
    this.send(s, 'pers', this.persFields(s, mine));
    this.send(s, '+who', this.userFields(s));
    this.send(s, '+sst', this.sstFields());
  }
  /** name's holder gives way to s when it is s's browser's own older session (its game logged in again) or its game
   *  is gone (a stale session) */
  giveWay(name, s) {
    const h = this.byPersona.get(name.toLowerCase());
    if (h && h !== s && (h.gone || h.vip === s.vip || this.now() - h.lastIn >= 30_000)) this.drop(h, 'the persona logged in again');
  }
  /** name is another player's: a session that holds it, or a player racing under it (a race this server started,
   *  the player not back yet) from another browser that is still on the switch, who comes back to it */
  nameTaken(name, s) {
    const k = name.toLowerCase(), h = this.byPersona.get(k);
    if (h && h !== s && !h.gone) return true;
    for (const g of this.games.values()) if (g.started) for (const p of g.players) {
      if (!p.back && p.vip !== s.vip && p.name.toLowerCase() === k && !this.goneVips.has(p.vip)) return true;
    }
    return false;
  }
  /** a free name: name with four digits in place of its end (a persona name has 15 characters at most); the same
   *  digits for one client while they are free (it keeps the name when it logs in again, even before its older
   *  session is gone), else random ones.  "Player" with the digits where the shortened name reads as a slur
   *  (names.mjs: "AbcdefghFagxyz" is "AbcdefghFag1234"): the game's next login would be refused under it */
  freeVariant(name, s) {
    const own = crypto.createHash('sha256').update(`${s.group}\n${name.toLowerCase()}`).digest().readUInt32BE(0) % 9000 + 1000;
    for (let i = 0; i < 20; i++) {
      const d = String(i ? crypto.randomInt(1000, 10000) : own), cut = name.slice(0, 15 - d.length) + d;
      const v = blockedName(cut) ? `Player${d}` : cut;
      this.giveWay(v, s);
      if (!this.nameTaken(v, s)) return v;
    }
    return null;
  }
  /** the lobby's counts (the Game Session Lobby's "Players Online" and "Games Created"; server.dll pushes them
   *  every few seconds) */
  sstFields() {
    let open = 0, racing = 0;
    for (const g of this.games.values()) g.started ? racing++ : open++;
    return [['UIL', this.byPersona.size], ['UIR', 0], ['UIG', 0], ['GIP', racing], ['GCR', open], ['GCM', 0]];
  }

  // ---- users and rooms ----
  /** a user record: the game form (+who, onln) */
  userFields(s) {
    const g = s.game ? this.games.get(s.game) : null;
    return [['I', s.id], ['N', s.persona], ['M', s.account], ['F', g ? 'G' : 'U'], ['A', s.addr], ['P', g ? 1 : 2], ['S', ''],
      ['X', s.auxi], ['G', g ? g.id : 0], ['AT', ''], ['CL', 511], ['LV', 0], ['MD', 0], ['LA', s.addr], ['HW', 0], ['RP', 0], ['MA', ''],
      ['US', ''], ['C', '']];
  }
  /** a user record as the others in a room see it (+usr) */
  usrFields(s) {
    return [['I', s.id], ['N', s.persona], ['M', s.account], ['F', 'H'], ['A', s.addr], ['P', 211], ['S', ''], ['X', s.auxi],
      ['G', s.game ?? 0], ['T', 2]];
  }
  roomFields(r) {
    return [['I', r.id], ['N', r.name], ['H', 'NFSU2'], ['F', 'CK'], ['T', r.users.size], ['L', ROOM_LIMIT], ['P', 0]];
  }
  roomOf(name) { for (const r of this.rooms.values()) if (r.name.toLowerCase() === String(name ?? '').toLowerCase()) return r; return null; }
  move(s, f) {
    const to = this.roomOf(f.get('NAME'));
    if (to && to.users.size >= ROOM_LIMIT && !to.users.has(s)) return this.send(s, 'move', [], 'full');
    if (!s.acts.take()) return this.send(s, 'move', [], 'full');
    const old = s.room;
    this.leaveRoom(s);
    if (!to) return this.send(s, 'move', [['IDENT', 0], ['NAME', ''], ['COUNT', 0], ['LIDENT', old ?? 0], ['LCOUNT', 0]]);
    to.users.add(s); s.room = to.id;
    this.send(s, 'move', [['IDENT', to.id], ['NAME', to.name], ['COUNT', to.users.size], ['FLAGS', 'C']]);
    this.send(s, '+who', [['I', s.id], ['N', s.persona], ['M', s.account], ['F', ''], ['A', s.addr], ['S', ''], ['X', s.auxi],
      ['R', to.name], ['RI', to.id], ['RF', 'C'], ['RT', 1]]);
    for (const u of to.users) {
      if (u === s) continue;
      this.send(s, '+usr', this.usrFields(u));
      this.send(u, '+usr', this.usrFields(s));
    }
    this.population(to);
  }
  leaveRoom(s) {
    const r = s.room ? this.rooms.get(s.room) : null;
    if (!r) return;
    r.users.delete(s); s.room = null;
    for (const u of r.users) {
      this.send(u, '+usr', [['I', s.id], ['T', 1]]);
      this.send(u, '+msg', [['F', 'C'], ['T', 'has left the room'], ['N', s.persona]]);
    }
    this.population(r);
  }
  /** a room's population to everyone, at most every announceMs (the latest) */
  population(r) {
    let a = this.pop.get(r.id);
    if (!a) this.pop.set(r.id, a = { at: -Infinity, timer: null, fn: null });
    this.later(a, this.L.announceMs, () => {
      const pop = [['Z', `${r.id}/${r.users.size}`]];
      for (const u of this.sessions) if (u.persona && !u.gone) this.send(u, '+pop', pop);
    });
  }
  auxi(s, f) {
    s.auxi = String(f.get('TEXT') ?? '').slice(0, this.L.auxiLen);
    this.send(s, 'auxi', [['TEXT', s.auxi]]);
    this.send(s, '+who', this.userFields(s));
    // to the others at actRate: past it they see it with the player's next record (a room, a game)
    if (!s.acts.take()) return;
    const r = s.room ? this.rooms.get(s.room) : null;
    if (r) for (const u of r.users) if (u !== s) this.send(u, '+usr', this.usrFields(s));
    const g = s.game ? this.games.get(s.game) : null;
    if (g) for (const p of g.players) if (p.s !== s && !p.s.gone) this.send(p.s, '+who', this.userFields(s));
  }
  /** a persona's online career (online now, or known from its races) */
  userInfo(s, f) {
    const p = String(f.get('PERS') ?? ''), u = this.byPersona.get(p.toLowerCase()), r = this.results.rec(p);
    if (!u && !r) return this.send(s, 'user', [], 'badc');
    const name = u ? u.persona : r.name, t = day(this.now());
    this.send(s, 'user', [['PERS', name], ['LAST', t], ['EXPR', 1072566000], ['STAT', this.results.statField(name)], ['CHEAT', 3], ['ACK_REP', 186],
      ['REP', 186], ['PLAST', t], ['PSINCE', t], ['DCNT', 0], ['ADDR', u ? u.addr : ''], ['SERV', 'NFSU2'], ['RANK', 99999], ['MESG', ''],
      ['LMSTAT', this.results.statField(name, true)], ['LGAME', '']]);
    this.send(s, 'auxi', [['TEXT', u ? u.auxi : '']]);
  }

  // ---- results and rankings ----
  /**
   * A player's report of a finished race.  Taken only for a race this server started (its AUTH), from one of
   * its players, once per player (the first that passes the checks) (the same names as the game's players, places
   * 1..n, the race and its times no shorter than the type's shortest race and no longer than the race has been
   * going).  The race's result is the first report that another player, in another browser, sent too (the
   * same places): one player alone cannot decide it; later reports are compared with it.  A ranked race of
   * fewer than three players (the game's own minimum) counts as unranked.  The game is told "Unranked" either
   * way (what the game shows after the race).
   */
  rank(s, f) {
    const reply = () => this.send(s, 'rank', [['RANK', 'Unranked'], ['TIME', Math.floor(this.now() / 1000) % 100000]]);
    const auth = f.get('AUTH') ?? '';
    const g = auth ? [...this.games.values()].find(x => x.auth === auth) : null;
    const me = g?.players.find(p => p.name.toLowerCase() === s.persona.toLowerCase());
    if (!g || !me) { this.log(`[online] ${this.who(s)}: a race report for no race of this server's (ignored)`, s.group); return reply(); }
    me.back = true;
    if (me.reported) return reply();
    const r = decodeResult(f.get('RESU'));
    const names = []; for (let i = 0; f.has(`NAME${i}`) && i < 8; i++) names.push(String(f.get(`NAME${i}`)));
    const why = this.checkResult(g, r, names);
    if (why) { this.log(`[online] "${g.name}": ${s.persona}'s race report not taken: ${why}`, s.group); reply(); return this.endIfBack(g); }
    me.reported = true;                                     // (a report not taken is not the player's one report)
    const order = r.players.map(p => ({ name: g.players.find(x => x.name.toLowerCase() === names[p.index].toLowerCase()).name,
      place: p.dropped ? 0 : p.place, time: p.time })).sort((a, b) => (a.place || 99) - (b.place || 99));
    const key = order.map(p => `${p.name}:${p.place}`).join();
    const line = order.map(p => `${p.place || 'DNF'} ${p.name}${p.place ? ` ${fmtTime(p.time)}` : ''}`).join(', ');
    if (g.result) {
      if (key !== g.result) this.log(`[online] "${g.name}": ${s.persona}'s report differs (${line}); the result stands`, s.group);
    } else if (g.reports.some(x => x.key === key && x.vip !== s.vip)) {
      const ranked = g.ranked && g.players.length >= 3;
      g.result = key;
      this.results.addRace({ type: r.type, ranked, track: r.track, players: order });
      this.log(`[online] "${g.name}" (${REPORT_TYPES[r.type]}, ${ranked ? 'ranked' : 'unranked'}): ${line}` +
        (r.type >= RACE_TYPES.length ? ' (no stats for this race type)' : g.ranked && !ranked ? ' (a ranked race needs 3 players: no points)' : ''), s.group);
    } else {
      if (g.reports.length && !g.reports.some(x => x.key === key)) this.log(`[online] "${g.name}": ${s.persona}'s report differs (${line})`, s.group);
      g.reports.push({ vip: s.vip, key });
    }
    reply();
    // the game asked for its career (user) at the login, before this report: once more, with the race in it
    this.userInfo(s, new Map([['PERS', s.persona]]));
    this.endIfBack(g);
  }
  /** why a decoded race report cannot be the result of game g ('' = it can) */
  checkResult(g, r, names) {
    if (!r) return 'the report does not parse';
    if (r.players.length !== g.players.length || names.length !== g.players.length) return 'not the game\'s number of players';
    const want = new Set(g.players.map(p => p.name.toLowerCase()));
    if (!names.every(n => want.delete(n.toLowerCase())) || want.size) return 'not the game\'s players';
    const idx = new Set(r.players.map(p => p.index)), places = r.players.filter(p => !p.dropped).map(p => p.place);
    if (idx.size !== names.length || [...idx].some(i => i >= names.length)) return 'players named twice';
    if (new Set(places).size !== places.length || places.some(p => p < 1 || p > names.length)) return 'places do not add up';
    if (!(r.type >= 0 && r.type < REPORT_TYPES.length)) return `race type ${r.type}`;
    const age = (this.now() - g.started) / 1000, min = MIN_RACE_S[r.type];
    if (age < min) return `a race of ${age.toFixed(1)} s (a ${REPORT_TYPES[r.type]} race takes ${min} s at least)`;
    if (r.players.some(p => !p.dropped && !(p.time >= min && p.time <= age + 5))) return 'a time the race has not had';
    return '';
  }
  /** back in the lobby after a race (its game list): the race is over for this player, who raced from this
   *  browser (another player under the same name is not that player: the race went on, and its result was lost) */
  backFromRace(s) {
    for (const g of this.games.values()) {
      if (!g.started) continue;
      const me = g.players.find(p => p.vip === s.vip && p.name.toLowerCase() === s.persona.toLowerCase());
      if (me && me.s !== s) { me.back = true; this.endIfBack(g); }
    }
  }
  endIfBack(g) { if (g.started && g.players.every(p => p.back)) this.endGame(g, 'every player is back in the lobby'); }
  /**
   * A ranking (the game's Rankings screens): CHAN 6..11 the overall board of one race type (RACE_TYPES
   * order), 12..17 the asking player's own row of it (RANGE=1 FIND=$<persona>, or "$" for one's own);
   * 18..29: the same for the current month (the game keeps channels 6..29: their meaning past 17 is
   * UNVERIFIED).  Rows: +snp N (name), R (rank), P (points), S (points,wins,losses), O; numbers in hex.
   */
  snap(s, f) {
    const chan = Number(f.get('CHAN')) | 0, index = f.get('INDEX') ?? 0;
    const start = Math.max(0, Number(f.get('START') ?? 0) | 0), range = Math.min(100, Math.max(0, Number(f.get('RANGE') ?? 0) | 0));
    const monthly = chan >= 18, type = (chan - 6) % 6, own = (chan - 6) % 12 >= 6;
    // the board as last built (stats.mjs: at most every few seconds), and its rows by name
    const board = chan >= 6 && chan <= 29 ? this.results.ranking(type, monthly) : { rows: [], index: new Map() };
    let rows;
    const find = f.get('FIND');
    if (own || find !== undefined) {
      const who = String(find ?? '').replace(/^\$/, '') || s.persona, i = board.index.get(who.toLowerCase());
      rows = i === undefined ? [] : [board.rows[i]];
    } else rows = board.rows.slice(start, start + range);
    this.send(s, 'snap', [['INDEX', index], ['CHAN', chan], ['START', start], ['RANGE', rows.length], ['SEQN', 0], ['COUNT', rows.length],
      ['TOTAL', board.rows.length], ['MORE', 0]]);
    const h = v => (v >>> 0).toString(16);
    for (const r of rows) this.send(s, '+snp', [['N', r.name], ['R', r.rank], ['P', h(r.points)], ['S', `${h(r.points)},${h(r.wins)},${h(r.losses)}`], ['O', 1]]);
  }
  online(s, f) {
    const u = this.byPersona.get(String(f.get('PERS') ?? '').toLowerCase());
    if (!u) return this.send(s, 'onln', [['I', 0], ['N', cleanText(f.get('PERS'), 16)]]);
    this.send(s, 'onln', this.userFields(u));
  }

  // ---- chat ----
  mesg(s, f) {
    const now = this.now();
    if (now < s.muteUntil) return this.send(s, 'mesg', [], 'mute');
    if (!s.chats.take()) {
      s.muteUntil = now + this.L.muteMs;
      this.log(`[online] ${this.who(s)}: chat flood, muted for ${this.L.muteMs / 1000} s`, s.group);
      return this.send(s, 'mesg', [], 'mute');
    }
    const text = cleanText(f.get('TEXT'), this.L.chatLen), priv = f.get('PRIV'), attr = String(f.get('ATTR') ?? '').slice(0, 8);
    this.send(s, 'mesg', attr ? [['ATTR', attr]] : []);
    if (!text && !attr) return;
    this.stats.chats++;
    if (priv) {                                             // to one persona (the game's challenge invites too)
      const u = this.byPersona.get(String(priv).toLowerCase());
      if (u && !u.gone) this.send(u, '+msg', [['F', 'P'], ['T', text], ['N', s.persona], ...(attr ? [['A', attr]] : [])]);
      return;
    }
    const msg = [['F', 'U'], ['T', text], ['N', s.persona], ...(attr ? [['A', attr]] : [])];
    // the game room's chat (ATTR G): the game's players; else the lobby room's
    const g = s.game ? this.games.get(s.game) : null;
    if (g && (attr.includes('G') || !s.room)) { for (const p of g.players) this.send(p.s, '+msg', msg); return; }
    const r = s.room ? this.rooms.get(s.room) : null;
    if (r) for (const u of r.users) this.send(u, '+msg', msg);
  }

  // ---- games ----
  gameFields(g, extra = []) {
    const f = [['IDENT', g.id], ['WHEN', g.when], ['NAME', g.name], ['HOST', g.hostName], ['ROOM', g.room], ['MAXSIZE', g.maxSize],
      ['MINSIZE', g.minSize], ['COUNT', g.players.length], ['CUSTFLAGS', g.custFlags], ['SYSFLAGS', g.sysFlags], ['EVID', 0], ['EVGID', 0],
      ['NUMPART', 1]];
    g.players.forEach((p, i) => f.push([`OPID${i}`, p.id], [`OPPO${i}`, p.name], [`ADDR${i}`, p.addr], [`LADDR${i}`, p.addr],
      [`MADDR${i}`, ''], [`OPPART${i}`, 0], [`OPFLAG${i}`, p.flags]));
    f.push(['PARTSIZE0', g.maxSize], ['PARAMS', g.params], ['PARTPARAMS0', '']);
    g.players.forEach((p, i) => f.push([`OPPARAM${i}`, '']));
    return [...f, ...extra];
  }
  gameByName(name) { for (const g of this.games.values()) if (g.name === name) return g; return null; }
  /**
   * The game list: every open game but a private one, those the player's Filter asks for first.  The game's search
   * asks for its Filter exactly (Game Mode, Collision Detection, Race Breaker, Car Performance Restrictions, Game
   * Type: CUSTFLAGS and SYSFLAGS under their masks), and EA's server took it so (server.dll 0x10017870), but with
   * this server's players a game of other settings than the Filter's default was seen by no one.
   */
  gameSearch(s, f) {
    const n = v => Number(v ?? 0) >>> 0;
    const cf = n(f.get('CUSTFLAGS')), cm = n(f.get('CUSTMASK')), sf = n(f.get('SYSFLAGS')), sm = n(f.get('SYSMASK'));
    const start = Math.max(0, Number(f.get('START') ?? 0) | 0), count = Math.min(50, Math.max(0, Number(f.get('COUNT') ?? 20) | 0));
    const asked = g => ((g.custFlags & cm) >>> 0) === ((cf & cm) >>> 0) && ((g.sysFlags & sm) >>> 0) === ((sf & sm) >>> 0);
    const open = [...this.games.values()].filter(g => !g.started && !(g.custFlags & PRIVATE) && (!s.room || !g.room || g.room === s.room));
    const list = [...open.filter(asked), ...open.filter(g => !asked(g))].slice(start, start + count);
    this.send(s, 'gsea', [['COUNT', list.length]]);
    for (const g of list) this.send(s, s.game === g.id ? '+mgm' : '+gam', this.gameFields(g));
  }
  /** games (open or started) whose creator is in s's browser, and in s's client */
  gamesOf(s, started) {
    let vip = 0, group = 0;
    for (const g of this.games.values()) {
      if (!g.started !== !started) continue;
      if (g.vip === s.vip) vip++;
      if (g.group === s.group) group++;
    }
    return { vip, group };
  }
  gameCreate(s, f) {
    if (s.game) this.gameLeave(s, null);                   // a game the player never left (a crash, a back key)
    if (this.games.size >= this.L.games || !s.creates.take()) return this.send(s, 'gcre', [], 'full');
    const mine = this.gamesOf(s, false);
    if (mine.vip >= this.L.openPerVip || mine.group >= this.L.openPerGroup) return this.send(s, 'gcre', [], 'full');
    const name = cleanText(f.get('NAME'), this.L.gameNameLen);
    if (!name) return this.send(s, 'gcre', [], 'inam');
    const password = String(f.get('PASS') ?? '');
    const sysFlags = (Number(f.get('SYSFLAGS') ?? 0) >>> 0) & ~ACTIVE;
    if ((sysFlags & PASSWORD) && !password) return this.send(s, 'gcre', [], 'pass');
    const passwordKey = password ? crypto.randomBytes(32) : null;
    const old = this.gameByName(name);
    if (old && old.hostName === s.persona && old.started) this.endGame(old, 'its creator made a new one');
    else if (old) return this.send(s, 'gcre', [], 'dupl');
    const g = {
      id: this.nextGame++, when: day(this.now()), name, hostName: s.persona, room: s.room ?? 0, vip: s.vip, group: s.group,
      maxSize: Math.min(this.L.gameSize, Math.max(2, Number(f.get('MAXSIZE') ?? 4) | 0)),
      minSize: Math.min(this.L.gameSize, Math.max(2, Number(f.get('MINSIZE') ?? 2) | 0)),
      passwordKey, passwordHash: passwordKey ? roomPassword(passwordKey, password) : null,
      custFlags: Number(f.get('CUSTFLAGS') ?? 0) >>> 0, sysFlags: ((sysFlags & ~PASSWORD) | (password ? PASSWORD : 0)) >>> 0,
      params: String(f.get('PARAMS') ?? '').slice(0, 256), players: [], started: 0, auth: '', reports: [], result: null,
      ranked: (sysFlags & RANKED) !== 0,
    };
    g.players.push(this.member(s));
    this.games.set(g.id, g); s.game = g.id; s.browsing = false;
    this.stats.games++;
    this.log(`[online] ${this.who(s)}: created the game "${name}" (${this.games.size} games)`, s.group);
    this.send(s, 'gcre', this.gameFields(g));
    this.send(s, '+who', this.userFields(s));
    this.send(s, '+mgm', this.gameFields(g));
    this.announce(g);
  }
  member(s) { return { s, id: s.id, name: s.persona, addr: s.addr, vip: s.vip, flags: 0, back: false, reported: false }; }
  /** a game's record to everyone looking at the game list in its room who is not in it (their lists update), at
   *  most every announceMs: the latest */
  announce(g, gone = false) {
    if (g.custFlags & PRIVATE) return;                     // not on the game list
    let a = this.agm.get(g.id);
    if (!a) this.agm.set(g.id, a = { at: -Infinity, timer: null, fn: null, gone: false });
    if (gone) a.gone = true;
    this.later(a, this.L.announceMs, () => {
      if (a.gone) this.agm.delete(g.id);
      const rec = a.gone || g.started ? [['IDENT', g.id]] : this.gameFields(g), r = this.rooms.get(g.room);
      for (const u of r ? r.users : this.sessions) {
        if (!u.persona || u.gone || !u.browsing || u.game === g.id) continue;
        this.send(u, '+agm', rec);
      }
    });
  }
  gameJoin(s, f) {
    if (!s.acts.take()) return this.send(s, 'gjoi', [], 'full');
    const g = this.gameByName(f.get('NAME') ?? '') ?? this.games.get(Number(f.get('IDENT')));
    if (!g || g.started) return this.send(s, 'gjoi', [], 'ugam');
    if (g.players.length >= g.maxSize) return this.send(s, 'gjoi', [], 'full');
    if (g.passwordHash && !crypto.timingSafeEqual(g.passwordHash, roomPassword(g.passwordKey, String(f.get('PASS') ?? ''))))
      return this.send(s, 'gjoi', [], 'pass');
    if (s.game === g.id) return this.send(s, 'gjoi', this.gameFields(g));
    if (s.game) this.gameLeave(s, null);
    g.players.push(this.member(s)); s.game = g.id; s.browsing = false;
    this.log(`[online] ${this.who(s)}: joined "${g.name}" (${g.players.length}/${g.maxSize})`, s.group);
    this.send(s, 'gjoi', this.gameFields(g));
    this.send(s, '+who', this.userFields(s));
    for (const p of g.players) if (p.s !== s) this.send(p.s, '+who', this.userFields(s));
    this.update(g);
  }
  /** a changed game to its players (+mgm) and the game list (+agm) */
  update(g) {
    const rec = this.gameFields(g);
    for (const p of g.players) this.send(p.s, '+mgm', rec);
    this.announce(g);
  }
  gameSet(s, f) {
    const g = s.game ? this.games.get(s.game) : null;
    if (!g) return this.send(s, 'gset', [], 'ugam');
    const p = g.players.find(x => x.s === s);
    let changed = false;
    const set = (o, k, v) => { if (o[k] !== v) { o[k] = v; changed = true; } };
    if (f.has('USERFLAGS')) set(p, 'flags', (Number(f.get('USERFLAGS')) >>> 0) & READY);
    if (s.persona === g.hostName && !g.started) {           // the creator may change the game's settings
      if (f.has('PARAMS')) set(g, 'params', String(f.get('PARAMS')).slice(0, 256));
      if (f.has('MAXSIZE')) set(g, 'maxSize', Math.min(this.L.gameSize, Math.max(g.players.length, 2, Number(f.get('MAXSIZE')) | 0)));
      if (f.has('MINSIZE')) set(g, 'minSize', Math.min(g.maxSize, Math.max(2, Number(f.get('MINSIZE')) | 0)));
      // of its SYSFLAGS only the countdown: ranked, password and started stay the server's
      if (f.has('SYSFLAGS')) set(g, 'sysFlags', ((g.sysFlags & ~COUNTDOWN) | ((Number(f.get('SYSFLAGS')) >>> 0) & COUNTDOWN)) >>> 0);
      // one name (the Kick button) or several, comma-separated (the players not ready when the countdown ends)
      const kicks = new Set(String(f.get('KICK') ?? '').split(',').map(n => n.trim().toLowerCase()).filter(Boolean));
      for (const kick of kicks) {
        const k = g.players.find(x => x.name.toLowerCase() === kick && x.s !== s);
        if (k) {
          this.log(`[online] "${g.name}": ${k.name} kicked out by ${s.persona}`, s.group);
          this.gameLeave(k.s, null);
          // its game learns it from the game's record that comes after its own record without the game: it leaves
          // the room (SPEED2.EXE 0x58dab0)
          this.send(k.s, '+who', this.userFields(k.s));
          this.send(k.s, '+mgm', this.gameFields(g));
        }
      }
    }
    this.send(s, 'gset', this.gameFields(g));
    // the others hear of a change only (a gset that changes nothing goes nowhere)
    if (changed) this.update(g);
  }
  gameStart(s, f) {
    const g = s.game ? this.games.get(s.game) : null;
    if (!g) return this.send(s, 'gsta', [], 'ugam');
    if (g.hostName !== s.persona) return this.send(s, 'gsta', [], 'host');
    if (g.players.length < g.minSize) return this.send(s, 'gsta', [], 'nepl');
    // one browser runs one game: players in the same browser are one player
    if (new Set(g.players.map(p => p.vip)).size !== g.players.length) return this.send(s, 'gsta', [], 'nepl');
    const mine = this.gamesOf(s, true);
    if (mine.vip >= this.L.startedPerVip || mine.group >= this.L.startedPerGroup) return this.send(s, 'gsta', [], 'full');
    g.started = this.now(); g.sysFlags = (g.sysFlags | ACTIVE) >>> 0;
    // the race's id for the result reports: the same for every player, never empty
    g.auth = crypto.createHash('md5').update(`${g.id}:${g.name}:${g.started}:${crypto.randomBytes(8).toString('hex')}`).digest('hex');
    this.stats.races++;
    const seed = crypto.randomInt(1, 2 ** 31);
    this.log(`[online] "${g.name}": the race starts (${g.players.map(p => p.name).join(', ')})`, s.group);
    this.send(s, 'gsta');
    this.update(g);
    for (const p of g.players) this.send(p.s, '+ses', this.gameFields(g, [['AUTH', g.auth], ['SEED', seed], ['SELF', p.name]]));
  }
  gameDelete(s, f) {
    const g = s.game ? this.games.get(s.game) : null;
    if (!g) return this.send(s, 'gdel', [], 'ugam');
    if (g.hostName !== s.persona) return this.send(s, 'gdel', [], 'host');
    this.send(s, 'gdel');
    this.endGame(g, 'deleted by its creator');
  }
  /** s leaves its game: kind = the reply's kind ('glea': back to the game list), or null (the session or the
   *  game went) */
  gameLeave(s, kind) {
    const g = s.game ? this.games.get(s.game) : null;
    s.game = null;
    if (kind) s.browsing = true;
    if (!g) return kind ? this.send(s, kind) : undefined;
    if (kind) this.send(s, kind);
    // a started race goes on without its players' lobby links (the game closes them at the start)
    if (g.started) { const p = g.players.find(x => x.s === s); if (p) p.s = { gone: true, persona: p.name }; return; }
    g.players = g.players.filter(p => p.s !== s);
    if (!g.players.length || g.hostName === s.persona) return this.endGame(g, g.players.length ? 'its creator left' : 'empty');
    this.update(g);
  }
  endGame(g, why) {
    if (!this.games.has(g.id)) return;
    this.games.delete(g.id);
    for (const p of g.players) {
      if (p.s.gone) continue;
      if (p.s.game === g.id) { p.s.game = null; p.s.browsing = true; }
      this.send(p.s, '+who', this.userFields(p.s));
      this.send(p.s, '+mgm', [['IDENT', g.id]]);
    }
    this.announce(g, true);
    this.log(`[online] the game "${g.name}" ended: ${why} (${this.games.size} games)`, g.group);
  }

  // ---- leaving, keep-alive ----
  leave(s, why) {
    if (s.gone) return;
    if (s.game) this.gameLeave(s, null);
    s.gone = true;
    this.sessions.delete(s);
    this.leaveRoom(s);
    if (s.persona && this.byPersona.get(s.persona.toLowerCase()) === s) this.byPersona.delete(s.persona.toLowerCase());
    if (s.account) this.log(`[online] ${this.who(s)}: left (${why}; ${this.sessions.size} connected)`, s.group);
  }
  tick() {
    const now = this.now();
    // what waits for its announceMs (the timers do it; here too for a clock that is not the real one)
    for (const a of [...this.agm.values(), ...this.pop.values()]) if (a.timer && now - a.at >= this.L.announceMs) this.fire(a);
    const sst = this.sstFields(), key = JSON.stringify(sst);
    if (key !== this.lastSst) { this.lastSst = key; for (const s of this.sessions) if (s.persona) this.send(s, '+sst', sst); }
    for (const s of [...this.sessions]) {
      if (now - s.lastIn > this.L.idleMs) { this.drop(s, `silent for ${Math.round((now - s.lastIn) / 1000)} s`); continue; }
      if (!s.persona && now - s.t0 > this.L.preLoginMs) { this.drop(s, 'no persona chosen'); continue; }
      if (now - s.lastPing >= this.L.pingMs) { s.lastPing = now; this.send(s, '~png', [['REF', day(now)]]); }
    }
    const away = vip => now - (this.goneVips.get(vip) ?? Infinity) > this.L.orphanMs;
    for (const g of [...this.games.values()]) {
      if (!g.started) continue;
      if (now - g.started > this.L.raceMs) this.endGame(g, 'the race is long over');
      else if (g.players.every(p => away(p.vip))) this.endGame(g, 'its players left the server');
    }
    for (const [v, t] of this.goneVips) if (now - t > this.L.raceMs) this.goneVips.delete(v);
  }
}
