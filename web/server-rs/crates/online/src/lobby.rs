//! The lobby of the online host: what EA's servers did for NFS Underground 2's Online mode, for the players of this
//! one server.
//!
//! Message shapes: those of EA's own lobby code, recorded from server.dll (the LAN server, which runs inside a
//! hosting browser) with the dev trace (trace.rs), plus the online-only parts (the config blob, rooms, personas, the
//! buddy login) from the game's code and what the game itself was seen to do.
//!
//! One session per lobby connection:
//! - connect -> the game sends addr, skey, news NAME=7 (asks for the config)
//! - sele -> what the session wants pushed
//! - auth -> any name logs in but one that reads as a slur (nfsu2-names; no accounts, nothing stored); PERSONAS
//! - acct -> "create account": logs in the same way
//! - cper/dper -> a persona for this session, or one less (at most `personas`)
//! - pers -> the persona to play as; LKEY.  One holder per name while online or racing (the holder's own browser
//!   takes it back): a name another browser holds is given with four digits in place of its end, and the game takes
//!   it for its own (runtime/online.c)
//! - move -> into a room (+who, +usr, +pop), or out (an unknown NAME)
//! - mesg -> chat in the room, or to one persona (PRIV; the game's challenge invites are private messages too): +msg
//! - gsea/gcre/gjoi/gset/gsta/gdel/glea -> the game list and one game: +gam, +mgm, +ses (the race starts: every
//!   player's virtual address)
//! - ~png -> the server pings every `ping_ms` and drops a session that says nothing for `idle_ms`
//! - user/onln -> a persona's record (user: the online career, STAT, from stats.rs)
//! - rank -> a player's report of a finished race (once per player and race, only for a race this server started,
//!   checked): the results; a ranked race's table on its players' results screens ($rup, $rfn)
//! - snap -> a ranking (the overall boards per race type) or one row of it
//!
//! At the race start the game closes its lobby link (as with server.dll in LAN play) and opens a new one afterwards
//! (it logs in again and reports the result: rank).  A started game therefore outlives its players' sessions, for
//! `race_ms` at most, and a restart of the server (races.rs).
//!
//! Never read: PASS (the account password), REGKEY (the CD key), PSES (a checksum of the player's files): aries.rs
//! passes over their values.  PASS in gcre and gjoi is a game's own password instead (SPEED2.EXE 0x58d5d0,
//! 0x58d7bb): read there, and kept only as a digest under a key of the game's own (RoomPassword), never in a reply,
//! the log or a file.  Never relayed: the game's MID (its adapter address).  Every address in a reply is the
//! browser's virtual one, which the switch wrote into the frame.
//!
//! What one client can make the lobby do to the others (the Online audit, ONL-2, ONL-5, ONL-7, ONL-9): sessions are
//! counted per browser, per client (its opaque group id from the switch) and per site, and the last `free_share` of
//! them are only for clients that hold none; a game's record goes to the players looking at the game list (since
//! their last gsea, until they are in a game), at most once a second per game, the latest; a room's population at
//! most once a second; a game's settings only when they changed; games created, joined and car descriptions
//! published have their own rates; a browser keeps `open_per_vip` open games and `started_per_vip` started ones (a
//! client `open_per_group`, `started_per_group`); a race needs players in different browsers, and is over once its
//! players' browsers have been gone from the switch for `orphan_ms`.  A race's result counts once two of its players
//! who finished it, in different browsers, reported the same places, no sooner than the race type's shortest race; a
//! ranked race needs three players for its points.
//!
//! Sessions and games are kept by id.  A session that left (and a game that ended) stays readable until the event
//! that ended it is over (the graveyard), so the code still handling that event goes on with it: a send to a gone
//! session does nothing, and the others may still hear of it.

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::sync::Arc;

use hmac::{Hmac, Mac};
use indexmap::{IndexMap, IndexSet};
use md5::Md5;
use nfsu2_util::{js, rand};
use sha2::{Digest, Sha256};

use crate::aries::{FieldMap, Fields, FrameReader, MAX_FRAME, V, encode_fields, encode_frame, is_secret};
use crate::core::Core;
use crate::jsx::{self, clean_text, day, lower, valid_name};
use crate::stats::{Race, RacePlayer, RaceReport, race_points};
use crate::timers::{Action, TimerId};
use crate::title::{Cfg, Title};
use crate::vhost::{ConnId, Svc};
use crate::{PORT_BUDDY, PORT_LOBBY};

pub type SessId = u64;
pub type GameId = u64;

/// The lobby's limits (all f64: the rules compare them with counts, rates and times alike)
#[derive(Clone, Copy, Debug)]
pub struct Limits {
    /// lobby sessions at once
    pub sessions: f64,
    /// lobby sessions from one browser (a reconnect overlaps the old one briefly)
    pub per_vip: f64,
    /// from one client (one address: a household's browsers)
    pub per_group: f64,
    /// from one site (an IPv6 /48)
    pub per_site: f64,
    /// the last quarter of the sessions: only for a client that holds none
    pub free_share: f64,
    /// messages a second from one session (burst: twice that)
    pub msg_rate: f64,
    /// chat messages a second (burst 5); over it: muted for mute_ms
    pub chat_rate: f64,
    /// characters of one chat message
    pub chat_len: f64,
    pub mute_ms: f64,
    /// games created a second by one session (burst 3)
    pub create_rate: f64,
    /// games joined, rooms entered and car descriptions published a second (burst 4)
    pub act_rate: f64,
    /// a game's record to the game list, a room's population: at most this often
    pub announce_ms: f64,
    /// per session (the game shows four)
    pub personas: f64,
    /// games at once
    pub games: f64,
    /// open games (not started) with a creator in one browser
    pub open_per_vip: f64,
    /// in one client
    pub open_per_group: f64,
    /// started games (until they end) with a creator in one browser
    pub started_per_vip: f64,
    /// in one client
    pub started_per_group: f64,
    /// players in one game (the race coordinator's racer entries; runtime/netrace.c)
    pub game_size: f64,
    pub game_name_len: f64,
    /// the car description a player publishes (auxi TEXT)
    pub auxi_len: f64,
    /// a ~png this often
    pub ping_ms: f64,
    /// nothing from the game for this long: the session goes
    pub idle_ms: f64,
    /// connected but no persona chosen for this long: goes
    pub pre_login_ms: f64,
    /// a started game is forgotten this long after the start
    pub race_ms: f64,
    /// a started game whose players' browsers left the switch this long ago ends
    pub orphan_ms: f64,
}

impl Default for Limits {
    fn default() -> Self {
        Limits {
            sessions: 3000.0,
            per_vip: 3.0,
            per_group: 8.0,
            per_site: 32.0,
            free_share: 0.25,
            msg_rate: 20.0,
            chat_rate: 1.0,
            chat_len: 255.0,
            mute_ms: 10_000.0,
            create_rate: 0.5,
            act_rate: 1.0,
            announce_ms: 1000.0,
            personas: 4.0,
            games: 900.0,
            open_per_vip: 2.0,
            open_per_group: 4.0,
            started_per_vip: 2.0,
            started_per_group: 8.0,
            game_size: 6.0,
            game_name_len: 32.0,
            auxi_len: 2048.0,
            ping_ms: 20_000.0,
            idle_ms: 120_000.0,
            pre_login_ms: 120_000.0,
            race_ms: 30.0 * 60_000.0,
            orphan_ms: 120_000.0,
        }
    }
}

/// sessions in one room at most
const ROOM_LIMIT: usize = 50;

/// games in one game list search: the game's page (its search asks for this many, START a page after another).  The
/// lobby shows one search's games until the next (the +agm updates never reach its rows), and a step past the last
/// row of a full page searches the next page (of a shorter list: the selection stays); at the game's own default, 20
/// (SPEED2.EXE 0x4bb14b), this server's list came in pages of 20 from searches seconds apart, which skipped and
/// repeated games and replaced the rows under the player ("Race lobby list is moving too fast", a player's report,
/// 2026-10-25).  This many: every open game in one search.
const GAMES_PER_UPDATE: usize = 200;

/// the server config (the "news" reply, code new7) the game asks for at connect: the title's (where the buddy server
/// is (here: this same host), the news and terms pages, the tier points per race type, the game list's page)
pub fn config_fields(t: &Title) -> Fields {
    t.config
        .iter()
        .map(|(k, v)| {
            let v = match v {
                Cfg::Text(s) => V::from(*s),
                Cfg::BuddyPort => V::from(PORT_BUDDY),
                Cfg::GamesPerUpdate => V::from(GAMES_PER_UPDATE),
            };
            (std::borrow::Cow::from(*k), v)
        })
        .collect()
}

/// a search's code (the header's [4..7], 0 in the game's own requests) when the player left the game's Filter screen
/// with the settings it asks for (runtime/olfilter.c)
const FILTER_SET: &str = "filt";

/// A token bucket: `rate` a second, at most `burst`, full at the start (on the lobby's clock)
#[derive(Clone, Copy, Debug)]
pub struct LBucket {
    pub rate: f64,
    pub cap: f64,
    pub v: f64,
    pub t: f64,
}

impl LBucket {
    pub fn new(rate: f64, burst: f64, now: f64) -> LBucket {
        LBucket { rate, cap: burst, v: burst, t: now }
    }
    pub fn take(&mut self, now: f64, n: f64) -> bool {
        self.v = self.cap.min(self.v + (now - self.t) / 1000.0 * self.rate);
        self.t = now;
        if self.v < n {
            return false;
        }
        self.v -= n;
        true
    }
}

pub struct Session {
    pub id: SessId,
    pub conn: ConnId,
    pub vip: u32,
    pub group: Arc<str>,
    pub site: Option<Arc<str>>,
    pub addr: String,
    reader: FrameReader,
    pub account: Option<String>,
    pub personas: Vec<String>,
    pub persona: Option<String>,
    pub lkey: Option<String>,
    pub room: Option<u32>,
    pub game: Option<GameId>,
    pub auxi: String,
    pub browsing: bool,
    t0: f64,
    pub last_in: f64,
    last_ping: f64,
    msgs: LBucket,
    chats: LBucket,
    creates: LBucket,
    acts: LBucket,
    mute_until: f64,
    pub gone: bool,
}

/// one player of a game.  `s`: its session, or None once a started race went on without its lobby link (the player
/// counts as gone; its name, address and report stay).  `results`: the session its race report came from (its
/// game's results screen, where a ranked race's table goes)
#[derive(Clone, Debug)]
pub struct Player {
    pub s: Option<SessId>,
    pub id: u64,
    pub name: String,
    pub addr: String,
    pub vip: u32,
    pub flags: u32,
    pub back: bool,
    pub reported: bool,
    pub results: Option<SessId>,
}

#[derive(Clone, Debug)]
pub struct Game {
    pub id: GameId,
    pub when: String,
    pub name: String,
    pub host_name: String,
    pub room: u32,
    pub vip: u32,
    pub group: Arc<str>,
    pub max_size: f64,
    pub min_size: f64,
    pub cust_flags: u32,
    pub sys_flags: u32,
    pub password: Option<RoomPassword>,
    pub params: String,
    pub players: Vec<Player>,
    /// when the race started (0: not started)
    pub started: f64,
    pub auth: String,
    pub reports: Vec<(u32, String)>,
    pub result: Option<String>,
    /// a ranked race's table once its result stands ($rfn, encoded once)
    pub table: Option<Vec<u8>>,
    pub ranked: bool,
}

/// a game's password as the lobby keeps it: an HMAC under a random key of the game's own, so that what the game
/// holds is no password and tells nothing of the same password in another game
#[derive(Clone, Debug)]
pub struct RoomPassword {
    key: [u8; 32],
    tag: [u8; 32],
}

impl RoomPassword {
    fn new(pass: &str) -> RoomPassword {
        let mut key = [0; 32];
        rand::fill(&mut key);
        let mut tag = [0; 32];
        tag.copy_from_slice(&Self::mac(&key, pass).finalize().into_bytes());
        RoomPassword { key, tag }
    }
    fn mac(key: &[u8; 32], pass: &str) -> Hmac<Sha256> {
        let mut m = Hmac::<Sha256>::new_from_slice(key).expect("any key length");
        m.update(pass.as_bytes());
        m
    }
    /// the same password (compared in constant time)
    pub fn matches(&self, pass: &str) -> bool {
        Self::mac(&self.key, pass).verify_slice(&self.tag).is_ok()
    }
}

pub struct Room {
    pub id: u32,
    pub name: &'static str,
    /// the sessions in it, in the order they came
    pub users: IndexSet<SessId>,
}

/// what `later` keeps for one record: when it last went (`at`), its timer, and whether one waits to go (`pending`;
/// what goes follows from its key)
#[derive(Clone, Copy, Debug)]
pub struct Later {
    at: f64,
    timer: Option<TimerId>,
    pending: bool,
}

impl Later {
    fn new() -> Later {
        Later { at: f64::NEG_INFINITY, timer: None, pending: false }
    }
}

/// a game's record to the game list: its `later`, whether the game is gone, and its room
struct Agm {
    later: Later,
    gone: bool,
    room: u32,
}

/// what a `later` timer does: a game's record to the game list, or a room's population
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum LaterKey {
    Agm(GameId),
    Pop(u32),
}

/// the lobby's counts (the server's info: core.rs `Core::info`)
#[derive(Clone, Copy, Debug, Default)]
pub struct LobbyStats {
    pub logins: u64,
    pub refused: u64,
    pub chats: u64,
    pub games: u64,
    pub races: u64,
    pub dropped: u64,
}

pub struct Lobby {
    pub l: Limits,
    /// the live sessions, in the order they came (ids grow)
    pub sessions: BTreeSet<SessId>,
    /// every session object still held: the live ones and those that left during the current event
    pub sess: HashMap<SessId, Session>,
    /// the live sessions of each browser (in the order they came), and how many each client and site holds: what a
    /// new lobby connection is counted against, and where a Messenger login looks for its key.  (Going through every
    /// session for both, as the Node server did, cost this thread 15-20 us with 1000 of them for a connection refused
    /// or a login with a key of nobody's: frames anyone can send, with no login.)
    by_vip: HashMap<u32, Vec<SessId>>,
    by_group: HashMap<Arc<str>, usize>,
    by_site: HashMap<Arc<str>, usize>,
    graveyard: Vec<SessId>,
    /// lower-case persona -> session (who holds the name now)
    pub by_persona: HashMap<String, SessId>,
    pub rooms: Vec<Room>,
    /// id -> game, in the order they were made
    pub games: BTreeMap<GameId, Game>,
    /// games that ended, while something still holds them (the current event, a game record not sent yet)
    dead: HashMap<GameId, Game>,
    /// game id -> its record to the game list, at most every announce_ms
    agm: IndexMap<GameId, Agm>,
    /// room id -> the same for its population
    pop: IndexMap<u32, Later>,
    /// a browser's address -> when it left the switch (not back since)
    pub gone_vips: HashMap<u32, f64>,
    pub next_user: u64,
    pub next_game: u64,
    pub stats: LobbyStats,
    last_sst: Option<[f64; 3]>,
}

impl Lobby {
    /// the lobby with the title's rooms
    pub fn new(l: Limits, rooms: &'static [&'static str]) -> Lobby {
        Lobby {
            l,
            sessions: BTreeSet::new(),
            sess: HashMap::new(),
            by_vip: HashMap::new(),
            by_group: HashMap::new(),
            by_site: HashMap::new(),
            graveyard: Vec::new(),
            by_persona: HashMap::new(),
            rooms: rooms.iter().enumerate().map(|(i, n)| Room { id: i as u32 + 1, name: n, users: IndexSet::new() }).collect(),
            games: BTreeMap::new(),
            dead: HashMap::new(),
            agm: IndexMap::new(),
            pop: IndexMap::new(),
            gone_vips: HashMap::new(),
            next_user: 1,
            next_game: 1,
            stats: LobbyStats::default(),
            last_sst: None,
        }
    }

    fn room(&self, id: u32) -> Option<&Room> {
        self.rooms.iter().find(|r| r.id == id)
    }
    fn room_mut(&mut self, id: u32) -> Option<&mut Room> {
        self.rooms.iter_mut().find(|r| r.id == id)
    }
    /// a game, live or ended (while held)
    pub fn game(&self, id: GameId) -> Option<&Game> {
        self.games.get(&id).or_else(|| self.dead.get(&id))
    }
    fn game_mut(&mut self, id: GameId) -> Option<&mut Game> {
        match self.games.get_mut(&id) {
            Some(g) => Some(g),
            None => self.dead.get_mut(&id),
        }
    }
    pub fn s(&self, id: SessId) -> &Session {
        &self.sess[&id]
    }
    fn sm(&mut self, id: SessId) -> &mut Session {
        self.sess.get_mut(&id).expect("a held session")
    }
    /// a held session is live now (`sessions.add`)
    fn add_live(&mut self, id: SessId) {
        let Some(s) = self.sess.get(&id) else { return };
        if !self.sessions.insert(id) {
            return;
        }
        self.by_vip.entry(s.vip).or_default().push(id);
        *self.by_group.entry(s.group.clone()).or_insert(0) += 1;
        if let Some(site) = &s.site {
            *self.by_site.entry(site.clone()).or_insert(0) += 1;
        }
    }

    /// a session is live no more (`sessions.delete`)
    fn remove_live(&mut self, id: SessId) {
        let Some(s) = self.sess.get(&id) else { return };
        if !self.sessions.remove(&id) {
            return;
        }
        if let Some(v) = self.by_vip.get_mut(&s.vip) {
            v.retain(|x| *x != id);
            if v.is_empty() {
                self.by_vip.remove(&s.vip);
            }
        }
        let less = |m: &mut HashMap<Arc<str>, usize>, k: &Arc<str>| {
            if let Some(n) = m.get_mut(k) {
                *n -= 1;
                if *n == 0 {
                    m.remove(k);
                }
            }
        };
        less(&mut self.by_group, &s.group);
        if let Some(site) = &s.site {
            less(&mut self.by_site, site);
        }
    }

    /// the live sessions of one browser, in the order they came
    pub fn of_vip(&self, vip: u32) -> &[SessId] {
        self.by_vip.get(&vip).map_or(&[], Vec::as_slice)
    }

    /// (tests) the sessions by browser, client and site are the live ones, counted again
    #[cfg(test)]
    pub fn check_counts(&self) {
        let mut v: HashMap<u32, Vec<SessId>> = HashMap::new();
        let (mut g, mut st): (HashMap<Arc<str>, usize>, HashMap<Arc<str>, usize>) = Default::default();
        for id in &self.sessions {
            let s = self.s(*id);
            assert!(!s.gone, "a live session that left");
            v.entry(s.vip).or_default().push(*id);
            *g.entry(s.group.clone()).or_insert(0) += 1;
            if let Some(x) = &s.site {
                *st.entry(x.clone()).or_insert(0) += 1;
            }
        }
        assert_eq!((v, g, st), (self.by_vip.clone(), self.by_group.clone(), self.by_site.clone()));
    }

    /// `p.s.gone`: a player without its session, or one whose session left
    fn player_gone(&self, p: &Player) -> bool {
        p.s.is_none_or(|s| self.sess.get(&s).is_none_or(|s| s.gone))
    }

    /// the end of an event: what left or ended is let go (a game whose record to the list is still to go stays)
    pub fn purge(&mut self) {
        for id in self.graveyard.drain(..) {
            self.sess.remove(&id);
        }
        let agm = &self.agm;
        self.dead.retain(|id, _| agm.contains_key(id));
    }

    /// racing: players of a started race not back in the lobby yet (their lobby link is closed while they race) and
    /// not online under their name again
    pub fn racers(&self, here: &HashSet<u32>) -> Vec<&Player> {
        let mut out = Vec::new();
        for g in self.games.values() {
            if g.started == 0.0 {
                continue;
            }
            for p in &g.players {
                if !p.back && !self.by_persona.contains_key(&lower(&p.name)) && here.contains(&p.vip) {
                    out.push(p);
                }
            }
        }
        out
    }

    /// the lobby's counts for the server's info: sessions, personas, racing, games (core.rs `Core::info` puts them
    /// after the virtual host's)
    pub fn info(&self, here: &HashSet<u32>) -> (usize, usize, usize, usize) {
        (self.sessions.len(), self.by_persona.len(), self.racers(here).len(), self.games.len())
    }

    /// the browsers in Online mode, as info() counts them: a persona in the lobby, or racing (the server's console)
    pub fn vips(&self, here: &HashSet<u32>) -> HashSet<u32> {
        let mut v: HashSet<u32> = self.by_persona.values().filter_map(|s| self.sess.get(s)).map(|s| s.vip).collect();
        v.extend(self.racers(here).iter().map(|p| p.vip));
        v
    }

    /// a frame from a browser: it is on the switch
    pub fn seen(&mut self, vip: u32) {
        if !self.gone_vips.is_empty() {
            self.gone_vips.remove(&vip);
        }
    }

    /// a browser left the switch
    pub fn peer_gone(&mut self, vip: u32, now: f64) {
        self.gone_vips.insert(vip, now);
    }

    fn later_mut(&mut self, k: LaterKey) -> Option<&mut Later> {
        match k {
            LaterKey::Agm(id) => self.agm.get_mut(&id).map(|a| &mut a.later),
            LaterKey::Pop(id) => self.pop.get_mut(&id),
        }
    }

    /// the lobby's counts (the Game Session Lobby's "Players Online" and "Games Created"; server.dll pushes them
    /// every few seconds)
    fn sst_counts(&self) -> [f64; 3] {
        let (mut open, mut racing) = (0, 0);
        for g in self.games.values() {
            if g.started != 0.0 {
                racing += 1;
            } else {
                open += 1;
            }
        }
        [self.by_persona.len() as f64, racing as f64, open as f64]
    }

    fn game_by_name(&self, name: &str) -> Option<GameId> {
        self.games.values().find(|g| g.name == name).map(|g| g.id)
    }

    /// games (open or started) whose creator is in s's browser, and in s's client
    fn games_of(&self, vip: u32, group: &str, started: bool) -> (f64, f64) {
        let (mut v, mut g) = (0.0, 0.0);
        for x in self.games.values() {
            if (x.started == 0.0) == started {
                continue;
            }
            if x.vip == vip {
                v += 1.0;
            }
            if &*x.group == group {
                g += 1.0;
            }
        }
        (v, g)
    }

    fn room_of(&self, name: Option<&str>) -> Option<u32> {
        let n = lower(name.unwrap_or(""));
        self.rooms.iter().find(|r| lower(r.name) == n).map(|r| r.id)
    }
}

/// a message kind: four printable ASCII characters, no space
pub fn four_printable(k: &str) -> bool {
    k.len() == 4 && k.bytes().all(|b| (b'!'..=b'~').contains(&b))
}

/// fields within max bytes of text: the optional ones (named in drop, first dropped first) go until it fits
fn fit(fields: Fields, max: usize, drop: &[&str]) -> Fields {
    let len = |f: &Fields| encode_fields(f, b'\n').map_or(0, |b| b.len());
    let mut f = fields;
    if len(&f) <= max {
        return f;
    }
    for k in drop {
        f.retain(|(n, _)| n != k);
        if len(&f) <= max {
            return f;
        }
    }
    f
}

/// a race time in a console line: "3:06.82"
pub(crate) fn fmt_time(t: f64) -> String {
    let m = (t / 60.0).floor();
    let s = js::to_fixed(t % 60.0, 2);
    format!("{}:{:0>5}", js::number_to_string(m), s)
}

/// a ranking's number in lowercase hex, as a u32 (`jsx::to_uint32`)
fn h(v: f64) -> String {
    format!("{:x}", jsx::to_uint32(v))
}

/// a session's persona as the console lines write it: "null" when there is none
fn tpl(s: &Option<String>) -> &str {
    s.as_deref().unwrap_or("null")
}

impl Core {
    fn now(&self) -> f64 {
        (self.clocks.now)()
    }

    /// a session's words to the game
    pub fn lsend(&mut self, s: SessId, kind: &str, fields: &[(std::borrow::Cow<'static, str>, V)], code: &str) {
        let Some(x) = self.lobby.sess.get(&s) else { return };
        if x.gone {
            return;
        }
        let conn = x.conn;
        match encode_frame(kind, code, fields) {
            Ok(f) => {
                self.conn_write(conn, &f);
            }
            // the lobby's own fields: never a bad name
            Err(e) => debug_assert!(false, "{e}"),
        }
    }

    /// a message encoded once, to one more session
    fn lsend_frame(&mut self, s: SessId, frame: &[u8]) {
        let Some(x) = self.lobby.sess.get(&s) else { return };
        if !x.gone {
            let conn = x.conn;
            self.conn_write(conn, frame);
        }
    }

    fn llog(&self, line: &str, s: SessId) {
        let group = self.lobby.s(s).group.clone();
        (self.log)(line, Some(&group));
    }

    // ---- the redirector (20920): @dir -> this lobby, and nothing else ----

    pub fn redirector_connect(&mut self, c: ConnId) {
        let t = self.set_timeout(30_000.0, Action::RedirectorIdle(c));
        self.vh.redirs.insert(c, crate::vhost::Redir { reader: FrameReader::default(), timer: Some(t) });
        self.set_svc(c, Svc::Redirector);
    }

    pub fn redirector_idle(&mut self, c: ConnId) {
        if self.vh.redirs.contains_key(&c) {
            self.conn_destroy(c, "redirector: no request");
        }
    }

    pub fn redirector_data(&mut self, c: ConnId, buf: &[u8]) {
        let Some(r) = self.vh.redirs.get_mut(&c) else { return };
        let msgs = r.reader.push(buf);
        let bad = r.reader.error.is_some();
        for m in msgs {
            // @tic asks for an RC4 ticket: no answer, so the lobby link stays plain (an 84-byte answer would key RC4)
            if m.kind != "@dir" {
                continue;
            }
            let f = fields![("ADDR", "10.77.0.1"), ("PORT", PORT_LOBBY), ("SESS", rand::int(1, 1 << 31)), ("MASK", rand::hex(16))];
            if let Ok(f) = encode_frame("@dir", "", &f) {
                self.conn_write(c, &f);
            }
            if let Some(t) = self.vh.redirs.get_mut(&c).and_then(|r| r.timer.take()) {
                self.timers.clear(t);
            }
            self.conn_end(c);
        }
        if bad {
            self.conn_destroy(c, "redirector: not the lobby protocol");
        }
    }

    // ---- the lobby (20922) ----

    pub fn lobby_connect(&mut self, c: ConnId) {
        let Some((vip, peer, group, site)) = self.conn_ids(c) else { return };
        // the live sessions of this browser, client and site
        let lb = &self.lobby;
        let nv = lb.of_vip(vip).len() as f64;
        let ng = lb.by_group.get(&group).copied().unwrap_or(0) as f64;
        let ns = site.as_ref().and_then(|x| lb.by_site.get(x)).copied().unwrap_or(0) as f64;
        let l = self.lobby.l;
        let n = self.lobby.sessions.len() as f64;
        let last_share = n >= l.sessions * (1.0 - l.free_share);
        if n >= l.sessions || nv >= l.per_vip || ng >= l.per_group || ns >= l.per_site || (last_share && (ng > 0.0 || ns > 0.0)) {
            self.lobby.stats.refused += 1;
            (self.log)(&format!("[online] {peer}: lobby full or too many sessions from that browser or client, refused"), Some(&group));
            return self.conn_destroy(c, "lobby full");
        }
        let t = self.now();
        let id = self.lobby.next_user;
        self.lobby.next_user += 1;
        self.lobby.sess.insert(
            id,
            Session {
                id,
                conn: c,
                vip,
                group,
                site,
                addr: peer,
                reader: FrameReader::new(MAX_FRAME),
                account: None,
                personas: Vec::new(),
                persona: None,
                lkey: None,
                room: None,
                game: None,
                auxi: String::new(),
                browsing: false,
                t0: t,
                last_in: t,
                last_ping: t,
                msgs: LBucket::new(l.msg_rate, 2.0 * l.msg_rate, t),
                chats: LBucket::new(l.chat_rate, 5.0, t),
                creates: LBucket::new(l.create_rate, 3.0, t),
                acts: LBucket::new(l.act_rate, 4.0, t),
                mute_until: 0.0,
                gone: false,
            },
        );
        self.lobby.add_live(id);
        self.lobby.gone_vips.remove(&vip);
        self.set_svc(c, Svc::Lobby(id));
    }

    pub fn lobby_data(&mut self, s: SessId, buf: &[u8]) {
        let now = self.now();
        let msgs = {
            let x = self.lobby.sm(s);
            x.last_in = now;
            x.reader.push(buf)
        };
        for m in msgs {
            if self.lobby.s(s).gone {
                return;
            }
            let now = self.now();
            if !self.lobby.sm(s).msgs.take(now, 1.0) {
                return self.ldrop(s, "too many messages");
            }
            if !four_printable(&m.kind) {
                return self.ldrop(s, "not the lobby protocol (a message kind that is not 4 letters)");
            }
            // PASS, REGKEY, PSES: never decoded (None); but PASS in gcre and gjoi, a game's password
            let f = if m.kind == "gcre" || m.kind == "gjoi" {
                FieldMap::parse_with(&m.body, |k| k != "PASS" && is_secret(k))
            } else {
                FieldMap::parse(&m.body)
            };
            self.message(s, &m.kind, &m.code, &f);
        }
        if let Some(e) = self.lobby.s(s).reader.error.clone() {
            self.ldrop(s, &format!("not the lobby protocol ({e})"));
        }
    }

    fn who(&self, s: SessId) -> String {
        let x = self.lobby.s(s);
        match (&x.persona, &x.account) {
            (Some(p), _) => format!("{} \"{p}\"", x.addr),
            (None, Some(a)) => format!("{} ({a})", x.addr),
            _ => x.addr.clone(),
        }
    }

    /// a session dropped: counted, on the console, its link reset (it leaves when the reset closes it)
    pub fn ldrop(&mut self, s: SessId, why: &str) {
        self.lobby.stats.dropped += 1;
        self.llog(&format!("[online] {}: dropped: {why}", self.who(s)), s);
        let conn = self.lobby.s(s).conn;
        self.conn_destroy(conn, why);
    }

    fn message(&mut self, s: SessId, kind: &str, code: &str, f: &FieldMap) {
        match kind {
            // ---- before the login ----
            // the game's own idea of its address (server.dll: no answer)
            "addr" => return,
            "skey" => return self.lsend(s, "skey", &fields![("SKEY", format!("${}", rand::hex(16)))], ""),
            "news" => {
                return if f.get("NAME") == Some("7") {
                    self.lsend(s, "news", &config_fields(self.title), "new7")
                } else {
                    self.lsend(s, "news", &[], "badc")
                };
            }
            "sele" => {
                let stats = 5000.0f64.min(jsx::to_int32(f.num("STATS", 0.0)) as f64);
                return self.lsend(
                    s,
                    "sele",
                    &fields![
                        ("GAMES", 1),
                        ("MYGAME", 1),
                        ("ROOMS", 1),
                        ("USERS", 1),
                        ("USERSETS", 0),
                        ("MESGS", 1),
                        ("ASYNC", 1),
                        ("RANKS", 0),
                        ("STATS", stats),
                        ("MORE", 1),
                        ("SLOTS", 36)
                    ],
                    "",
                );
            }
            // keep-alive answers (last_in is enough)
            "~png" | "@alv" | "@cnt" => return,
            "auth" | "acct" => return self.auth(s, kind, f),
            // nothing to recover: there are no accounts
            "lost" => return self.lsend(s, "lost", &[], "imst"),
            // ---- personas ----
            "cper" => return self.create_persona(s, f),
            "dper" => return self.delete_persona(s, f),
            "pers" => return self.select_persona(s, f),
            _ => {}
        }
        // everything else needs a persona
        if self.lobby.s(s).persona.is_none() {
            return self.lsend(s, kind, &[], "auth");
        }
        match kind {
            "move" => self.move_room(s, f),
            "mesg" => self.mesg(s, f),
            "auxi" => self.auxi(s, f),
            "user" => self.user_info(s, f.get("PERS").unwrap_or("")),
            "onln" => self.online(s, f),
            // the game list: this player looks at it now (and gets its changes) until it is in a game
            "gsea" => {
                self.lobby.sm(s).browsing = true;
                self.back_from_race(s);
                self.game_search(s, f, code == FILTER_SET)
            }
            "gcre" => self.game_create(s, f),
            "gjoi" => self.game_join(s, f),
            "gset" => self.game_set(s, f),
            "gsta" => self.game_start(s),
            "gdel" => self.game_delete(s),
            "glea" => self.game_leave(s, Some("glea")),
            "rank" => self.rank(s, f),
            "snap" => self.snap(s, f),
            "rept" => {
                self.llog(&format!("[online] {}: a complaint about \"{}\"", self.who(s), clean_text(f.str("PERS"), 16)), s);
                self.lsend(s, "rept", &fields![("TEXT", "Report complete")], "")
            }
            _ => self.lsend(s, kind, &[], "badc"),
        }
    }

    // ---- login ----

    fn auth(&mut self, s: SessId, kind: &str, f: &FieldMap) {
        let name = f.str("NAME").to_string();
        if self.lobby.s(s).account.is_some() {
            return self.lsend(s, kind, &[], "logn");
        }
        // a name that reads as a slur (nfsu2-names) as one the game would not let one type
        if !valid_name(&name) || nfsu2_names::blocked_name(&name) {
            return self.lsend(s, kind, &[], if kind == "acct" { "inam" } else { "imst" });
        }
        let x = self.lobby.sm(s);
        x.account = Some(name.clone());
        // the account's personas: the name itself (cper adds more for this session)
        x.personas = vec![name.clone()];
        let addr = x.addr.clone();
        self.lobby.stats.logins += 1;
        self.llog(&format!("[online] {addr}: logged in as \"{name}\" ({} connected)", self.lobby.sessions.len()), s);
        let t = day(self.now());
        // the game keeps the reply's text in 122 bytes: the optional fields go first
        let personas = self.lobby.s(s).personas.join(",");
        let reply = fit(
            fields![
                ("NAME", name),
                ("PERSONAS", personas),
                ("TOS", 1),
                ("ADDR", addr),
                ("LAST", t),
                ("BORN", "19800101"),
                ("SPAM", "NN"),
                ("MAIL", "player@localhost")
            ],
            122,
            &["MAIL", "SPAM", "BORN", "LAST"],
        );
        self.lsend(s, kind, &reply, "");
        for i in 0..self.lobby.rooms.len() {
            let r = self.room_fields(self.lobby.rooms[i].id);
            self.lsend(s, "+rom", &r, "");
        }
    }

    fn create_persona(&mut self, s: SessId, f: &FieldMap) {
        if self.lobby.s(s).account.is_none() {
            return self.lsend(s, "cper", &[], "auth");
        }
        let p = f.str("PERS").to_string();
        if !valid_name(&p) || nfsu2_names::blocked_name(&p) {
            return self.lsend(s, "cper", &[], "invp");
        }
        let lp = lower(&p);
        if self.lobby.s(s).personas.iter().any(|x| lower(x) == lp) || self.lobby.by_persona.contains_key(&lp) {
            return self.lsend(s, "cper", &[], "dupl");
        }
        if self.lobby.s(s).personas.len() as f64 >= self.lobby.l.personas {
            return self.lsend(s, "cper", &[], "nspc");
        }
        self.lobby.sm(s).personas.push(p.clone());
        let r = self.pers_fields(s, &p);
        self.lsend(s, "cper", &r, "");
    }

    fn delete_persona(&mut self, s: SessId, f: &FieldMap) {
        if self.lobby.s(s).account.is_none() {
            return self.lsend(s, "dper", &[], "auth");
        }
        let p = f.str("PERS").to_string();
        let lp = lower(&p);
        let x = self.lobby.s(s);
        let i = x.personas.iter().position(|y| lower(y) == lp);
        let current = x.persona.as_ref().is_some_and(|q| lower(q) == lp);
        let Some(i) = i.filter(|_| !current) else {
            return self.lsend(s, "dper", &fields![("PERS", p), ("RESULT", 1)], "");
        };
        self.lobby.sm(s).personas.remove(i);
        self.lsend(s, "dper", &fields![("PERS", p), ("RESULT", 0)], "");
    }

    /// the pers / cper reply (the game keeps it in 108 bytes)
    fn pers_fields(&self, s: SessId, p: &str) -> Fields {
        let t = day(self.now());
        let x = self.lobby.s(s);
        fit(
            fields![("PERS", p), ("LKEY", x.lkey.clone().unwrap_or_default()), ("NAME", &x.account), ("LAST", t.clone()), ("PLAST", t)],
            108,
            &["PLAST", "LAST", "NAME"],
        )
    }

    fn select_persona(&mut self, s: SessId, f: &FieldMap) {
        if self.lobby.s(s).account.is_none() {
            return self.lsend(s, "pers", &[], "auth");
        }
        let lp = lower(f.str("PERS"));
        let Some(mine) = self.lobby.s(s).personas.iter().find(|x| lower(x) == lp).cloned() else {
            return self.lsend(s, "pers", &[], "invp");
        };
        self.give_way(&mine, s);
        // one holder per name, online or racing: a name another player holds is this player's with digits.  The
        // answer names it, and the runtime puts it in the game's own persona and account fields (runtime/online.c):
        // the game finds itself among a race's players by them (one that kept the name it asked for raced against
        // itself)
        let asked = mine.clone();
        let mine = if self.name_taken(&mine, s) {
            match self.free_variant(&mine, s) {
                Some(v) => v,
                None => return self.lsend(s, "pers", &[], "pset"),
            }
        } else {
            mine
        };
        let old = self.lobby.s(s).persona.clone();
        if let Some(old) = old.filter(|o| *o != mine) {
            self.lobby.by_persona.remove(&lower(&old));
        }
        // the key of the buddy login: the game keeps 31 characters of it (its buddy AUTH carried 31 of 32)
        let x = self.lobby.sm(s);
        x.persona = Some(mine.clone());
        x.lkey = Some(rand::hex(15));
        let addr = x.addr.clone();
        self.lobby.by_persona.insert(lower(&mine), s);
        let taken = if mine != asked { format!(" (\"{asked}\" is taken)") } else { String::new() };
        self.llog(&format!("[online] {addr}: plays as \"{mine}\"{taken}"), s);
        let r = self.pers_fields(s, &mine);
        self.lsend(s, "pers", &r, "");
        let w = self.user_fields(s);
        self.lsend(s, "+who", &w, "");
        let sst = self.sst_fields();
        self.lsend(s, "+sst", &sst, "");
    }

    /// name's holder gives way to s when it is s's browser's own older session (its game logged in again) or its
    /// game is gone (a stale session)
    fn give_way(&mut self, name: &str, s: SessId) {
        let Some(h) = self.lobby.by_persona.get(&lower(name)).copied() else { return };
        if h == s {
            return;
        }
        let Some(hx) = self.lobby.sess.get(&h) else { return };
        if hx.gone || hx.vip == self.lobby.s(s).vip || self.now() - hx.last_in >= 30_000.0 {
            self.ldrop(h, "the persona logged in again");
        }
    }

    /// name is another player's: a session that holds it, or a player racing under it (a race this server started,
    /// the player not back yet) from another browser that is still on the switch, who comes back to it
    fn name_taken(&self, name: &str, s: SessId) -> bool {
        let k = lower(name);
        if let Some(h) = self.lobby.by_persona.get(&k)
            && *h != s
            && self.lobby.sess.get(h).is_some_and(|h| !h.gone)
        {
            return true;
        }
        let vip = self.lobby.s(s).vip;
        for g in self.lobby.games.values() {
            if g.started == 0.0 {
                continue;
            }
            for p in &g.players {
                if !p.back && p.vip != vip && lower(&p.name) == k && !self.lobby.gone_vips.contains_key(&p.vip) {
                    return true;
                }
            }
        }
        false
    }

    /// a free name: name with four digits in place of its end (a persona name has 15 characters at most); the same
    /// digits for one client while they are free (it keeps the name when it logs in again, even before its older
    /// session is gone), else random ones.  "Player" with the digits where the shortened name reads as a slur
    /// ("AbcdefghFagxyz" is "AbcdefghFag1234"): the game's next login would be refused under it
    fn free_variant(&mut self, name: &str, s: SessId) -> Option<String> {
        let digest = Sha256::digest(format!("{}\n{}", self.lobby.s(s).group, lower(name)).as_bytes());
        let own = u32::from_be_bytes([digest[0], digest[1], digest[2], digest[3]]) % 9000 + 1000;
        for i in 0..20 {
            let d = if i == 0 { own.to_string() } else { rand::int(1000, 10000).to_string() };
            let cut = format!("{}{d}", jsx::slice16(name, 15 - d.len()));
            let v = if nfsu2_names::blocked_name(&cut) { format!("Player{d}") } else { cut };
            if nfsu2_names::blocked_name(&v) {
                continue; // digits that are no name either (1488): other ones
            }
            self.give_way(&v, s);
            if !self.name_taken(&v, s) {
                return Some(v);
            }
        }
        None
    }

    fn sst_fields(&self) -> Fields {
        let [uil, gip, gcr] = self.lobby.sst_counts();
        fields![("UIL", uil), ("UIR", 0), ("UIG", 0), ("GIP", gip), ("GCR", gcr), ("GCM", 0)]
    }

    // ---- users and rooms ----

    /// a user record: the game form (+who, onln)
    fn user_fields(&self, s: SessId) -> Fields {
        let x = self.lobby.s(s);
        let g = x.game.and_then(|id| self.lobby.games.get(&id));
        fields![
            ("I", x.id),
            ("N", &x.persona),
            ("M", &x.account),
            ("F", if g.is_some() { "G" } else { "U" }),
            ("A", &x.addr),
            ("P", if g.is_some() { 1 } else { 2 }),
            ("S", ""),
            ("X", &x.auxi),
            ("G", g.map_or(0, |g| g.id)),
            ("AT", ""),
            ("CL", 511),
            ("LV", 0),
            ("MD", 0),
            ("LA", &x.addr),
            ("HW", 0),
            ("RP", 0),
            ("MA", ""),
            ("US", ""),
            ("C", "")
        ]
    }

    /// a user record as the others in a room see it (+usr)
    fn usr_fields(&self, s: SessId) -> Fields {
        let x = self.lobby.s(s);
        fields![
            ("I", x.id),
            ("N", &x.persona),
            ("M", &x.account),
            ("F", "H"),
            ("A", &x.addr),
            ("P", 211),
            ("S", ""),
            ("X", &x.auxi),
            ("G", x.game.unwrap_or(0)),
            ("T", 2)
        ]
    }

    fn room_fields(&self, id: u32) -> Fields {
        let r = self.lobby.room(id).expect("a room");
        fields![
            ("I", r.id),
            ("N", r.name),
            ("H", self.title.room_h),
            ("F", self.title.room_f),
            ("T", r.users.len()),
            ("L", ROOM_LIMIT),
            ("P", 0)
        ]
    }

    fn move_room(&mut self, s: SessId, f: &FieldMap) {
        let to = self.lobby.room_of(f.get("NAME"));
        if let Some(to) = to {
            let r = self.lobby.room(to).expect("a room");
            if r.users.len() >= ROOM_LIMIT && !r.users.contains(&s) {
                return self.lsend(s, "move", &[], "full");
            }
        }
        let now = self.now();
        if !self.lobby.sm(s).acts.take(now, 1.0) {
            return self.lsend(s, "move", &[], "full");
        }
        let old = self.lobby.s(s).room;
        self.leave_room(s);
        let Some(to) = to else {
            return self.lsend(
                s,
                "move",
                &fields![("IDENT", 0), ("NAME", ""), ("COUNT", 0), ("LIDENT", old.unwrap_or(0)), ("LCOUNT", 0)],
                "",
            );
        };
        self.lobby.room_mut(to).expect("a room").users.insert(s);
        self.lobby.sm(s).room = Some(to);
        let (name, count) = {
            let r = self.lobby.room(to).expect("a room");
            (r.name, r.users.len())
        };
        self.lsend(s, "move", &fields![("IDENT", to), ("NAME", name), ("COUNT", count), ("FLAGS", "C")], "");
        let x = self.lobby.s(s);
        let who = fields![
            ("I", x.id),
            ("N", &x.persona),
            ("M", &x.account),
            ("F", ""),
            ("A", &x.addr),
            ("S", ""),
            ("X", &x.auxi),
            ("R", name),
            ("RI", to),
            ("RF", "C"),
            ("RT", 1)
        ];
        self.lsend(s, "+who", &who, "");
        let users: Vec<SessId> = self.lobby.room(to).expect("a room").users.iter().copied().collect();
        for u in users {
            if u == s || !self.lobby.room(to).is_some_and(|r| r.users.contains(&u)) {
                continue;
            }
            let fu = self.usr_fields(u);
            self.lsend(s, "+usr", &fu, "");
            let fs = self.usr_fields(s);
            self.lsend(u, "+usr", &fs, "");
        }
        self.population(to);
    }

    fn leave_room(&mut self, s: SessId) {
        let Some(r) = self.lobby.s(s).room.filter(|r| self.lobby.room(*r).is_some()) else { return };
        self.lobby.room_mut(r).expect("a room").users.shift_remove(&s);
        self.lobby.sm(s).room = None;
        let (id, persona) = {
            let x = self.lobby.s(s);
            (x.id, x.persona.clone())
        };
        let usr = encode_frame("+usr", "", &fields![("I", id), ("T", 1)]).expect("the lobby's own fields");
        let msg =
            encode_frame("+msg", "", &fields![("F", "C"), ("T", "has left the room"), ("N", &persona)]).expect("the lobby's own fields");
        let users: Vec<SessId> = self.lobby.room(r).expect("a room").users.iter().copied().collect();
        for u in users {
            if !self.lobby.room(r).is_some_and(|x| x.users.contains(&u)) {
                continue;
            }
            self.lsend_frame(u, &usr);
            self.lsend_frame(u, &msg);
        }
        self.population(r);
    }

    /// a room's population to everyone, at most every announce_ms (the latest)
    fn population(&mut self, r: u32) {
        self.lobby.pop.entry(r).or_insert_with(Later::new);
        self.later(LaterKey::Pop(r));
    }

    fn send_population(&mut self, r: u32) {
        let pop = fields![("Z", format!("{r}/{}", self.lobby.room(r).map_or(0, |x| x.users.len())))];
        let pop = encode_frame("+pop", "", &pop).expect("the lobby's own fields");
        let all: Vec<SessId> = self.lobby.sessions.iter().copied().collect();
        for u in all {
            let Some(x) = self.lobby.sess.get(&u) else { continue };
            if !self.lobby.sessions.contains(&u) || x.persona.is_none() || x.gone {
                continue;
            }
            self.lsend_frame(u, &pop);
        }
    }

    /// a record out now, or (only the latest) once announce_ms has passed since the last one
    fn later(&mut self, k: LaterKey) {
        let (ms, now) = (self.lobby.l.announce_ms, self.now());
        let Some(a) = self.lobby.later_mut(k) else { return };
        a.pending = true;
        if a.timer.is_some() {
            return;
        }
        let wait = a.at + ms - now;
        if wait <= 0.0 {
            return self.fire(k);
        }
        let t = self.set_timeout(wait, Action::Later(k));
        if let Some(a) = self.lobby.later_mut(k) {
            a.timer = Some(t);
        }
    }

    /// a `later` record goes now (when one waits), and its timer ends
    pub fn fire(&mut self, k: LaterKey) {
        let now = self.now();
        let Some(a) = self.lobby.later_mut(k) else { return };
        let t = a.timer.take();
        a.at = now;
        let pending = std::mem::take(&mut a.pending);
        if let Some(t) = t {
            self.timers.clear(t);
        }
        if pending {
            match k {
                LaterKey::Pop(r) => self.send_population(r),
                LaterKey::Agm(g) => self.send_agm(g),
            }
        }
    }

    /// a `later` timer fired
    pub fn later_timer(&mut self, k: LaterKey, id: TimerId) {
        if self.lobby.later_mut(k).is_some_and(|a| a.timer == Some(id)) {
            self.fire(k);
        }
    }

    fn auxi(&mut self, s: SessId, f: &FieldMap) {
        let text = jsx::slice16(f.str("TEXT"), self.lobby.l.auxi_len as usize).to_string();
        self.lobby.sm(s).auxi = text.clone();
        self.lsend(s, "auxi", &fields![("TEXT", text)], "");
        let w = self.user_fields(s);
        self.lsend(s, "+who", &w, "");
        // to the others at act_rate: past it they see it with the player's next record (a room, a game)
        let now = self.now();
        if !self.lobby.sm(s).acts.take(now, 1.0) {
            return;
        }
        if let Some(r) = self.lobby.s(s).room.filter(|r| self.lobby.room(*r).is_some()) {
            let users: Vec<SessId> = self.lobby.room(r).expect("a room").users.iter().copied().collect();
            for u in users {
                if u != s && self.lobby.room(r).is_some_and(|x| x.users.contains(&u)) {
                    let fs = self.usr_fields(s);
                    self.lsend(u, "+usr", &fs, "");
                }
            }
        }
        if let Some(g) = self.lobby.s(s).game.and_then(|g| self.lobby.games.get(&g)) {
            let players = g.players.clone();
            for p in players {
                if p.s != Some(s) && !self.lobby.player_gone(&p) {
                    let w = self.user_fields(s);
                    self.lsend(p.s.expect("not gone"), "+who", &w, "");
                }
            }
        }
    }

    /// a persona's online career (online now, or known from its races)
    fn user_info(&mut self, s: SessId, pers: &str) {
        let u = self.lobby.by_persona.get(&lower(pers)).copied();
        let r = self.stats.rec(pers, false).map(|r| r.name.clone());
        if u.is_none() && r.is_none() {
            return self.lsend(s, "user", &[], "badc");
        }
        let name = match u {
            Some(u) => self.lobby.s(u).persona.clone().unwrap_or_default(),
            None => r.unwrap_or_default(),
        };
        let t = day(self.now());
        let stat = self.stats.stat_field(Some(&name), false);
        let lmstat = self.stats.stat_field(Some(&name), true);
        let addr = u.map_or(String::new(), |u| self.lobby.s(u).addr.clone());
        self.lsend(
            s,
            "user",
            &fields![
                ("PERS", name),
                ("LAST", t.clone()),
                ("EXPR", 1072566000),
                ("STAT", stat),
                ("CHEAT", 3),
                ("ACK_REP", 186),
                ("REP", 186),
                ("PLAST", t.clone()),
                ("PSINCE", t),
                ("DCNT", 0),
                ("ADDR", addr),
                ("SERV", self.title.serv),
                ("RANK", 99999),
                ("MESG", ""),
                ("LMSTAT", lmstat),
                ("LGAME", "")
            ],
            "",
        );
        let auxi = u.map_or(String::new(), |u| self.lobby.s(u).auxi.clone());
        self.lsend(s, "auxi", &fields![("TEXT", auxi)], "");
    }

    // ---- results and rankings ----

    /// A player's report of a finished race.  Taken only for a race this server started (its AUTH), from one of its
    /// players, once per player (the first that passes the checks) (the same names as the game's players, places
    /// 1..n, the race and its times no shorter than the type's shortest race and no longer than the race has been
    /// going).  The race's result is the first report that another player, in another browser, sent too (the same
    /// places): one player alone cannot decide it; later reports are compared with it.  Only the reports of players who
    /// finished decide it: the game of a player who left the race (dropped out, quit) reports every player as dropped
    /// and is back first, so two of them decided a fifth of the live results as "everyone DNF" before the finishers
    /// reported (2026-10-24); such a report is logged, and its player sees the table.  A ranked race of fewer than
    /// three players (the game's own minimum) counts as unranked.  The game is told "Unranked" either way (what the
    /// game shows after the race); a ranked game's players see its table on their results screens (`results_table`).
    fn rank(&mut self, s: SessId, f: &FieldMap) {
        let reply = |core: &mut Core| {
            let time = (core.now() / 1000.0).floor() % 100_000.0;
            core.lsend(s, "rank", &fields![("RANK", "Unranked"), ("TIME", time)], "");
        };
        let auth = f.str("AUTH");
        let persona = lower(tpl(&self.lobby.s(s).persona));
        let g = if auth.is_empty() { None } else { self.lobby.games.values().find(|x| x.auth == auth).map(|g| g.id) };
        let me = g.and_then(|g| self.lobby.game(g)?.players.iter().position(|p| lower(&p.name) == persona));
        let (Some(g), Some(me)) = (g, me) else {
            self.llog(&format!("[online] {}: a race report for no race of this server's (ignored)", self.who(s)), s);
            return reply(self);
        };
        let gm = self.lobby.game_mut(g).expect("held");
        gm.players[me].back = true;
        gm.players[me].results = Some(s);
        if gm.players[me].reported {
            reply(self);
            return self.results_table(g, Some(s));
        }
        let r = (self.title.decode_report)(f.get("RESU").unwrap_or("undefined"));
        let mut names = Vec::new();
        let mut i = 0;
        while f.has(&format!("NAME{i}")) && i < 8 {
            names.push(f.get(&format!("NAME{i}")).unwrap_or("null").to_string());
            i += 1;
        }
        let why = self.check_result(g, r.as_ref(), &names);
        let spers = tpl(&self.lobby.s(s).persona).to_string();
        let gname = self.lobby.game(g).expect("held").name.clone();
        if !why.is_empty() {
            self.llog(&format!("[online] \"{gname}\": {spers}'s race report not taken: {why}"), s);
            reply(self);
            self.results_table(g, Some(s));
            return self.end_if_back(g);
        }
        let r = r.expect("checked");
        // the reporter's own entry in its report (check_result: each index names one of the game's players)
        let own = r.players.iter().find(|p| !p.dropped && lower(&names[p.index as usize]) == persona);
        let finished = own.is_some();
        // its records: what its own game measured of it (stats.rs add_records), whether the result stands or not
        if let Some(p) = own {
            let name = self.lobby.game(g).expect("held").players[me].name.clone();
            self.stats.add_records(&name, r.kind as u32, p);
        }
        // (a report not taken is not the player's one report)
        let gm = self.lobby.game_mut(g).expect("held");
        gm.players[me].reported = true;
        let mut order: Vec<RacePlayer> = r
            .players
            .iter()
            .map(|p| {
                let want = lower(&names[p.index as usize]);
                let name = gm.players.iter().find(|x| lower(&x.name) == want).map(|x| x.name.clone()).unwrap_or_default();
                RacePlayer { name, place: if p.dropped { 0 } else { p.place as u32 }, time: p.time }
            })
            .collect();
        order.sort_by_key(|p| if p.place == 0 { 99 } else { p.place });
        let key = order.iter().map(|p| format!("{}:{}", p.name, p.place)).collect::<Vec<_>>().join(",");
        let line = order
            .iter()
            .map(|p| {
                let place = if p.place == 0 { "DNF".to_string() } else { p.place.to_string() };
                let time = if p.place != 0 { format!(" {}", fmt_time(p.time)) } else { String::new() };
                format!("{place} {}{time}", p.name)
            })
            .collect::<Vec<_>>()
            .join(", ");
        let svip = self.lobby.s(s).vip;
        let gm = self.lobby.game(g).expect("held");
        // the ranked table goes to this player's results screen alone once the result stands, else to every reporter's
        let mut to = None;
        if !finished {
            if gm.result.is_some() {
                to = Some(s);
            }
            self.llog(&format!("[online] \"{gname}\": {spers} did not finish: the report decides nothing ({line})"), s);
        } else if let Some(result) = &gm.result {
            to = Some(s);
            if key != *result {
                self.llog(&format!("[online] \"{gname}\": {spers}'s report differs ({line}); the result stands"), s);
            }
        } else if gm.reports.iter().any(|(v, k)| *k == key && *v != svip) {
            let ranked = gm.ranked && gm.players.len() >= 3;
            let was_ranked = gm.ranked;
            let table = gm.ranked.then(|| self.table_frame(g, &r, &names, ranked && (r.kind as usize) < self.title.race_types.len()));
            let gm = self.lobby.game_mut(g).expect("held");
            gm.result = Some(key);
            gm.table = table;
            self.stats.add_race(&Race { kind: r.kind as u32, ranked, track: r.track as i64, players: order });
            let tail = if r.kind as usize >= self.title.race_types.len() {
                " (no stats for this race type)"
            } else if was_ranked && !ranked {
                " (a ranked race needs 3 players: no points)"
            } else {
                ""
            };
            let rt = self.title.report_types[r.kind as usize];
            self.llog(&format!("[online] \"{gname}\" ({rt}, {}): {line}{tail}", if ranked { "ranked" } else { "unranked" }), s);
        } else {
            if !gm.reports.is_empty() && !gm.reports.iter().any(|(_, k)| *k == key) {
                self.llog(&format!("[online] \"{gname}\": {spers}'s report differs ({line})"), s);
            }
            self.lobby.game_mut(g).expect("held").reports.push((svip, key));
        }
        reply(self);
        // the game asked for its career (user) at the login, before this report: once more, with the race in it
        let pers = self.lobby.s(s).persona.clone().unwrap_or_default();
        self.user_info(s, &pers);
        self.results_table(g, to);
        self.end_if_back(g);
    }

    /// A ranked race's table on its players' results screens (UI_PostRace_TournResults.fng), to one session or (None)
    /// to every player's that reported: the table once the result stands ($rfn), until then which players' reports
    /// were taken ($rup: OPPOn the name, STATn 1 "received" or 0 "waiting", TIME the countdown in ms).  The game
    /// hands a lobby message whose kind starts with '$' to its race handler (SPEED2.EXE 0x7470b2, 0x58dab0), which
    /// fills the screen's table (0x4c0430); without one the table stayed empty.  Unranked races show the game's own
    /// results.
    fn results_table(&mut self, g: GameId, to: Option<SessId>) {
        let Some(gm) = self.lobby.game(g) else { return };
        if !gm.ranked {
            return;
        }
        let frame = match &gm.table {
            Some(t) => t.clone(),
            None => {
                let mut f: Fields = Vec::new();
                for (i, p) in gm.players.iter().take(6).enumerate() {
                    f.extend(fields![(format!("OPPO{i}"), p.name.as_str()), (format!("STAT{i}"), u32::from(p.reported))]);
                }
                f.extend(fields![("TIME", self.title.results_wait_ms)]);
                encode_frame("$rup", "", &f).expect("the lobby's own fields")
            }
        };
        let sessions: Vec<SessId> = match to {
            Some(s) => vec![s],
            None => gm.players.iter().filter_map(|p| p.results).collect(),
        };
        for s in sessions {
            self.lsend_frame(s, &frame);
        }
    }

    /// The ranked table ($rfn) from the report that decided the result: per player of the game (OPPOn, at most the
    /// table's 6) DATAn, a list in hex (SPEED2.EXE 0x58df4f, 0x7425d0 "l*"): the time in ms (drift: the score), the
    /// place (-2 "Disconnected": dropped out), two numbers the screen does not show (0), the rating points (0 when the
    /// race gives none)
    fn table_frame(&self, g: GameId, r: &RaceReport, names: &[String], points: bool) -> Vec<u8> {
        let hex = |x: i64| if x < 0 { format!("-{:x}", -x) } else { format!("{x:x}") };
        let n = r.players.len();
        let mut f: Fields = Vec::new();
        for (i, pl) in self.lobby.game(g).expect("held").players.iter().take(6).enumerate() {
            let want = lower(&pl.name);
            let Some(p) = r.players.iter().find(|p| lower(&names[p.index as usize]) == want) else { continue };
            let place = if p.dropped { 0 } else { p.place as u32 };
            let drift = self.title.report_types[r.kind as usize] == "drift";
            let first = if drift { p.score as i64 } else { (p.time * 1000.0).round().clamp(0.0, 1e9) as i64 };
            let pts = race_points(points, n, place) as i64;
            let data = format!("{},{},0,0,{}", hex(first), hex(if place == 0 { -2 } else { place as i64 }), hex(pts));
            f.extend(fields![(format!("OPPO{i}"), pl.name.as_str()), (format!("DATA{i}"), data)]);
        }
        encode_frame("$rfn", "", &f).expect("the lobby's own fields")
    }

    /// why a decoded race report cannot be the result of game g ('' = it can)
    fn check_result(&self, g: GameId, r: Option<&crate::stats::RaceReport>, names: &[String]) -> String {
        let Some(r) = r else { return "the report does not parse".into() };
        let g = self.lobby.game(g).expect("held");
        if r.players.len() != g.players.len() || names.len() != g.players.len() {
            return "not the game's number of players".into();
        }
        let mut want: HashSet<String> = g.players.iter().map(|p| lower(&p.name)).collect();
        if !names.iter().all(|n| want.remove(&lower(n))) || !want.is_empty() {
            return "not the game's players".into();
        }
        let idx: HashSet<u8> = r.players.iter().map(|p| p.index).collect();
        let places: Vec<u8> = r.players.iter().filter(|p| !p.dropped).map(|p| p.place).collect();
        if idx.len() != names.len() || idx.iter().any(|i| *i as usize >= names.len()) {
            return "players named twice".into();
        }
        let distinct: HashSet<u8> = places.iter().copied().collect();
        if distinct.len() != places.len() || places.iter().any(|p| *p < 1 || *p as usize > names.len()) {
            return "places do not add up".into();
        }
        if r.kind as usize >= self.title.report_types.len() {
            return format!("race type {}", r.kind);
        }
        let age = (self.now() - g.started) / 1000.0;
        let min = self.title.min_race_s[r.kind as usize];
        if age < min {
            return format!(
                "a race of {} s (a {} race takes {} s at least)",
                js::to_fixed(age, 1),
                self.title.report_types[r.kind as usize],
                js::number_to_string(min)
            );
        }
        if r.players.iter().any(|p| !p.dropped && !(p.time >= min && p.time <= age + 5.0)) {
            return "a time the race has not had".into();
        }
        String::new()
    }

    /// back in the lobby after a race (its game list): the race is over for this player, who raced from this
    /// browser (another player under the same name is not that player: the race went on, and its result was lost)
    fn back_from_race(&mut self, s: SessId) {
        let (vip, persona) = {
            let x = self.lobby.s(s);
            (x.vip, lower(tpl(&x.persona)))
        };
        let ids: Vec<GameId> = self.lobby.games.keys().copied().collect();
        for id in ids {
            let Some(g) = self.lobby.games.get_mut(&id) else { continue };
            if g.started == 0.0 {
                continue;
            }
            let Some(me) = g.players.iter_mut().find(|p| p.vip == vip && lower(&p.name) == persona) else { continue };
            if me.s != Some(s) {
                me.back = true;
                self.end_if_back(id);
            }
        }
    }

    fn end_if_back(&mut self, g: GameId) {
        let Some(x) = self.lobby.game(g) else { return };
        if x.started != 0.0 && x.players.iter().all(|p| p.back) {
            self.end_game(g, "every player is back in the lobby");
        }
    }

    /// A ranking (the game's Rankings screens, SPEED2.EXE 0x58c700 and 0x58c990, the channel from 0x5826e0): CHAN
    /// 6..11 a race type's overall board (Overall Rankings), 12..17 the asking player's own row of it (RANGE=1
    /// FIND=$<persona>, or "$" for one's own), 18..23 the current month's board (Current Month's Leaderboard, its first
    /// two), 24..29 last month's (Last Month's Leaderboard: none, the server keeps no past month).  Each six in the
    /// screens' order, Drift before Drag (the title's `slot_types`; until 2026-10-25 read as race types, so the Drift
    /// pages showed the drag board); INDEX (the race type and the player's class, 0x58ce30) only goes back.  Rows:
    /// +snp N (name), R (rank), P (points), S (points,wins,losses), O.  R and P in decimal (SPEED2.EXE 0x7420b0 reads
    /// digits 0-9 only; the screen shows P, sorts its list by it and takes the player's class from it), S in hex
    /// (0x7425d0, "l*"; the screen shows its wins and losses).
    fn snap(&mut self, s: SessId, f: &FieldMap) {
        let chan = jsx::to_int32(f.num("CHAN", f64::NAN));
        let index = f.get("INDEX").map_or(V::N(0.0), V::from);
        let start = 0i32.max(jsx::to_int32(f.num("START", 0.0)));
        let range = 100i32.min(0i32.max(jsx::to_int32(f.num("RANGE", 0.0))));
        // (in 64 bits: CHAN - 6 does not overflow for a CHAN near -2^31)
        let at = chan as i64 - 6;
        let kind = (0..18).contains(&at).then(|| self.title.slot_types[(at % 6) as usize]);
        let (own, monthly) = ((6..12).contains(&at), at >= 12);
        let find = f.get("FIND").map(str::to_string);
        // the board as last built (stats.rs: at most every few seconds), and its rows by name
        let (rows, total) = {
            let persona = self.lobby.s(s).persona.clone().unwrap_or_default();
            let board = match kind {
                Some(k) => Some(self.stats.ranking(k, monthly)),
                None => None,
            };
            let all: &[crate::stats::BoardRow] = board.map_or(&[], |b| &b.rows);
            let rows: Vec<crate::stats::BoardRow> = if own || find.is_some() {
                let f = find.unwrap_or_default();
                let w = f.strip_prefix('$').unwrap_or(&f);
                let who = if w.is_empty() { persona } else { w.to_string() };
                match board.and_then(|b| b.index.get(&lower(&who))) {
                    Some(i) => vec![all[*i].clone()],
                    None => Vec::new(),
                }
            } else {
                let (a, b) = (start as usize, start as usize + range as usize);
                all.get(a.min(all.len())..b.min(all.len())).unwrap_or(&[]).to_vec()
            };
            (rows, all.len())
        };
        self.lsend(
            s,
            "snap",
            &fields![
                ("INDEX", index),
                ("CHAN", chan),
                ("START", start),
                ("RANGE", rows.len()),
                ("SEQN", 0),
                ("COUNT", rows.len()),
                ("TOTAL", total),
                ("MORE", 0)
            ],
            "",
        );
        for r in rows {
            self.lsend(
                s,
                "+snp",
                &fields![
                    ("N", r.name),
                    ("R", r.rank),
                    ("P", jsx::to_uint32(r.points)),
                    ("S", format!("{},{},{}", h(r.points), h(r.wins), h(r.losses))),
                    ("O", 1)
                ],
                "",
            );
        }
    }

    fn online(&mut self, s: SessId, f: &FieldMap) {
        match self.lobby.by_persona.get(&lower(f.str("PERS"))).copied() {
            None => self.lsend(s, "onln", &fields![("I", 0), ("N", clean_text(f.str("PERS"), 16))], ""),
            Some(u) => {
                let w = self.user_fields(u);
                self.lsend(s, "onln", &w, "")
            }
        }
    }

    // ---- chat ----

    fn mesg(&mut self, s: SessId, f: &FieldMap) {
        let now = self.now();
        if now < self.lobby.s(s).mute_until {
            return self.lsend(s, "mesg", &[], "mute");
        }
        if !self.lobby.sm(s).chats.take(now, 1.0) {
            let mute = self.lobby.l.mute_ms;
            self.lobby.sm(s).mute_until = now + mute;
            self.llog(&format!("[online] {}: chat flood, muted for {} s", self.who(s), js::number_to_string(mute / 1000.0)), s);
            return self.lsend(s, "mesg", &[], "mute");
        }
        let text = clean_text(f.str("TEXT"), self.lobby.l.chat_len as usize);
        let attr = jsx::slice16(f.str("ATTR"), 8).to_string();
        self.lsend(s, "mesg", &if attr.is_empty() { vec![] } else { fields![("ATTR", attr.clone())] }, "");
        if text.is_empty() && attr.is_empty() {
            return;
        }
        self.lobby.stats.chats += 1;
        let persona = self.lobby.s(s).persona.clone();
        let with_attr = |mut v: Fields| {
            if !attr.is_empty() {
                v.extend(fields![("A", attr.clone())]);
            }
            v
        };
        // to one persona (the game's challenge invites too)
        if let Some(priv_) = f.get("PRIV").filter(|p| !p.is_empty()) {
            if let Some(u) = self.lobby.by_persona.get(&lower(priv_)).copied()
                && !self.lobby.s(u).gone
            {
                self.lsend(u, "+msg", &with_attr(fields![("F", "P"), ("T", text), ("N", &persona)]), "");
            }
            return;
        }
        let msg = encode_frame("+msg", "", &with_attr(fields![("F", "U"), ("T", text), ("N", &persona)])).expect("the lobby's own fields");
        // the game room's chat (ATTR G): the game's players; else the lobby room's
        let room = self.lobby.s(s).room;
        if let Some(g) = self.lobby.s(s).game.and_then(|g| self.lobby.games.get(&g))
            && (attr.contains('G') || room.is_none())
        {
            let players: Vec<Option<SessId>> = g.players.iter().map(|p| p.s).collect();
            for p in players.into_iter().flatten() {
                self.lsend_frame(p, &msg);
            }
            return;
        }
        if let Some(r) = room.filter(|r| self.lobby.room(*r).is_some()) {
            let users: Vec<SessId> = self.lobby.room(r).expect("a room").users.iter().copied().collect();
            for u in users {
                if self.lobby.room(r).is_some_and(|x| x.users.contains(&u)) {
                    self.lsend_frame(u, &msg);
                }
            }
        }
    }

    // ---- games ----

    fn game_fields(&self, g: GameId, extra: Fields) -> Fields {
        let g = self.lobby.game(g).expect("held");
        let mut f = fields![
            ("IDENT", g.id),
            ("WHEN", &g.when),
            ("NAME", &g.name),
            ("HOST", &g.host_name),
            ("ROOM", g.room),
            ("MAXSIZE", g.max_size),
            ("MINSIZE", g.min_size),
            ("COUNT", g.players.len()),
            ("CUSTFLAGS", g.cust_flags),
            ("SYSFLAGS", g.sys_flags),
            ("EVID", 0),
            ("EVGID", 0),
            ("NUMPART", 1)
        ];
        for (i, p) in g.players.iter().enumerate() {
            f.extend(fields![
                (format!("OPID{i}"), p.id),
                (format!("OPPO{i}"), &p.name),
                (format!("ADDR{i}"), &p.addr),
                (format!("LADDR{i}"), &p.addr),
                (format!("MADDR{i}"), ""),
                (format!("OPPART{i}"), 0),
                (format!("OPFLAG{i}"), p.flags)
            ]);
        }
        f.extend(fields![("PARTSIZE0", g.max_size), ("PARAMS", &g.params), ("PARTPARAMS0", "")]);
        for i in 0..g.players.len() {
            f.extend(fields![(format!("OPPARAM{i}"), "")]);
        }
        f.extend(extra);
        f
    }

    /// The game list: every open game but a private one, those the player's Filter asks for first; only those once
    /// the player set the Filter (`filter_set`: the search's code FILTER_SET), unless it asks for its defaults.  The
    /// game's search asks for its Filter exactly (Game Mode, Collision Detection, Race Breaker, Car Performance
    /// Restrictions, Game Type: CUSTFLAGS and SYSFLAGS under their masks), and EA's server took it so (server.dll
    /// 0x10017870), but with this server's players a game of other settings than the Filter's default was seen by no
    /// one.  The game's Filter is also the settings of the game it last made or joined, so only the runtime can tell
    /// one the player set.
    fn game_search(&mut self, s: SessId, f: &FieldMap, filter_set: bool) {
        let n = |k: &str| jsx::to_uint32(f.num(k, 0.0));
        let (cf, cm, sf, sm) = (n("CUSTFLAGS"), n("CUSTMASK"), n("SYSFLAGS"), n("SYSMASK"));
        let private = self.title.flags.private;
        let only_asked = filter_set && [cf & cm, cm, sf & sm, sm] != self.title.filter_default;
        let start = 0i32.max(jsx::to_int32(f.num("START", 0.0))) as usize;
        let count = (0i32.max(jsx::to_int32(f.num("COUNT", 20.0))) as usize).min(GAMES_PER_UPDATE);
        let x = self.lobby.s(s);
        let (room, mine) = (x.room, x.game);
        let asked = |g: &Game| (g.cust_flags & cm) == (cf & cm) && (g.sys_flags & sm) == (sf & sm);
        let open: Vec<&Game> = self
            .lobby
            .games
            .values()
            .filter(|g| g.started == 0.0 && g.cust_flags & private == 0 && (room.is_none() || g.room == 0 || Some(g.room) == room))
            .collect();
        let list: Vec<GameId> = open
            .iter()
            .filter(|g| asked(g))
            .chain(open.iter().filter(|g| !only_asked && !asked(g)))
            .skip(start)
            .take(count)
            .map(|g| g.id)
            .collect();
        self.lsend(s, "gsea", &fields![("COUNT", list.len())], "");
        for g in list {
            let gf = self.game_fields(g, vec![]);
            self.lsend(s, if mine == Some(g) { "+mgm" } else { "+gam" }, &gf, "");
        }
    }

    fn game_create(&mut self, s: SessId, f: &FieldMap) {
        // a game the player never left (a crash, a back key)
        if self.lobby.s(s).game.is_some() {
            self.game_leave(s, None);
        }
        let now = self.now();
        if self.lobby.games.len() as f64 >= self.lobby.l.games || !self.lobby.sm(s).creates.take(now, 1.0) {
            return self.lsend(s, "gcre", &[], "full");
        }
        let (vip, group) = {
            let x = self.lobby.s(s);
            (x.vip, x.group.clone())
        };
        let (mv, mg) = self.lobby.games_of(vip, &group, false);
        if mv >= self.lobby.l.open_per_vip || mg >= self.lobby.l.open_per_group {
            return self.lsend(s, "gcre", &[], "full");
        }
        let name = clean_text(f.str("NAME"), self.lobby.l.game_name_len as usize);
        if name.is_empty() {
            return self.lsend(s, "gcre", &[], "inam");
        }
        let persona = self.lobby.s(s).persona.clone().unwrap_or_default();
        if let Some(old) = self.lobby.game_by_name(&name) {
            let o = &self.lobby.games[&old];
            if o.host_name == persona && o.started != 0.0 {
                self.end_game(old, "its creator made a new one");
            } else {
                return self.lsend(s, "gcre", &[], "dupl");
            }
        }
        let fl = self.title.flags;
        let sys_flags = jsx::to_uint32(f.num("SYSFLAGS", 0.0)) & !fl.active;
        let pass = f.str("PASS");
        let password = (!pass.is_empty()).then(|| RoomPassword::new(pass));
        let l = self.lobby.l;
        let size = |k: &str, def: f64| l.game_size.min(2f64.max(jsx::to_int32(f.num(k, def)) as f64));
        let id = self.lobby.next_game;
        self.lobby.next_game += 1;
        let x = self.lobby.s(s);
        let g = Game {
            id,
            when: day(now),
            name: name.clone(),
            host_name: persona,
            room: x.room.unwrap_or(0),
            vip: x.vip,
            group: x.group.clone(),
            max_size: size("MAXSIZE", 4.0),
            min_size: size("MINSIZE", 2.0),
            cust_flags: jsx::to_uint32(f.num("CUSTFLAGS", 0.0)),
            sys_flags: (sys_flags & !fl.password) | if password.is_some() { fl.password } else { 0 },
            password,
            params: jsx::slice16(f.str("PARAMS"), 256).to_string(),
            players: vec![self.member(s)],
            started: 0.0,
            auth: String::new(),
            reports: Vec::new(),
            result: None,
            table: None,
            ranked: sys_flags & fl.ranked != 0,
        };
        self.lobby.games.insert(id, g);
        let x = self.lobby.sm(s);
        x.game = Some(id);
        x.browsing = false;
        self.lobby.stats.games += 1;
        self.llog(&format!("[online] {}: created the game \"{name}\" ({} games)", self.who(s), self.lobby.games.len()), s);
        let gf = self.game_fields(id, vec![]);
        self.lsend(s, "gcre", &gf, "");
        let w = self.user_fields(s);
        self.lsend(s, "+who", &w, "");
        let gf = self.game_fields(id, vec![]);
        self.lsend(s, "+mgm", &gf, "");
        self.announce(id, false);
    }

    fn member(&self, s: SessId) -> Player {
        let x = self.lobby.s(s);
        Player {
            s: Some(s),
            id: x.id,
            name: x.persona.clone().unwrap_or_default(),
            addr: x.addr.clone(),
            vip: x.vip,
            flags: 0,
            back: false,
            reported: false,
            results: None,
        }
    }

    /// a game's record to everyone looking at the game list in its room who is not in it (their lists update), at
    /// most every announce_ms: the latest
    fn announce(&mut self, g: GameId, gone: bool) {
        let Some(x) = self.lobby.game(g) else { return };
        // not on the game list
        if x.cust_flags & self.title.flags.private != 0 {
            return;
        }
        let room = x.room;
        let a = self.lobby.agm.entry(g).or_insert_with(|| Agm { later: Later::new(), gone: false, room });
        if gone {
            a.gone = true;
        }
        self.later(LaterKey::Agm(g));
    }

    fn send_agm(&mut self, g: GameId) {
        let Some(a) = self.lobby.agm.get(&g) else { return };
        let (gone, room) = (a.gone, a.room);
        if gone {
            self.lobby.agm.shift_remove(&g);
        }
        let rec = match self.lobby.game(g) {
            Some(x) if !gone && x.started == 0.0 => self.game_fields(g, vec![]),
            _ => fields![("IDENT", g)],
        };
        let rec = encode_frame("+agm", "", &rec).expect("the lobby's own fields");
        let in_room = self.lobby.room(room).is_some();
        let targets: Vec<SessId> = if in_room {
            self.lobby.room(room).expect("a room").users.iter().copied().collect()
        } else {
            self.lobby.sessions.iter().copied().collect()
        };
        for u in targets {
            let still =
                if in_room { self.lobby.room(room).is_some_and(|r| r.users.contains(&u)) } else { self.lobby.sessions.contains(&u) };
            let Some(x) = self.lobby.sess.get(&u) else { continue };
            if !still || x.persona.is_none() || x.gone || !x.browsing || x.game == Some(g) {
                continue;
            }
            self.lsend_frame(u, &rec);
        }
    }

    fn game_join(&mut self, s: SessId, f: &FieldMap) {
        let now = self.now();
        if !self.lobby.sm(s).acts.take(now, 1.0) {
            return self.lsend(s, "gjoi", &[], "full");
        }
        let ident = f.num("IDENT", f64::NAN);
        let by_id = (ident.is_finite() && ident >= 0.0 && ident.fract() == 0.0)
            .then_some(ident as u64)
            .filter(|id| self.lobby.games.contains_key(id));
        let Some(g) = self.lobby.game_by_name(f.str("NAME")).or(by_id) else { return self.lsend(s, "gjoi", &[], "ugam") };
        let x = &self.lobby.games[&g];
        if x.started != 0.0 {
            return self.lsend(s, "gjoi", &[], "ugam");
        }
        if x.players.len() as f64 >= x.max_size {
            return self.lsend(s, "gjoi", &[], "full");
        }
        if x.password.as_ref().is_some_and(|p| !p.matches(f.str("PASS"))) {
            return self.lsend(s, "gjoi", &[], "pass");
        }
        // a join refused leaves the player where it was; one into the game it is in changes nothing
        match self.lobby.s(s).game {
            Some(cur) if cur == g => {
                let gf = self.game_fields(g, vec![]);
                return self.lsend(s, "gjoi", &gf, "");
            }
            // a game the player never left (a crash, a back key)
            Some(_) => self.game_leave(s, None),
            None => {}
        }
        let m = self.member(s);
        let x = self.lobby.games.get_mut(&g).expect("live");
        x.players.push(m);
        let (gname, n, max) = (x.name.clone(), x.players.len(), x.max_size);
        let xs = self.lobby.sm(s);
        xs.game = Some(g);
        xs.browsing = false;
        self.llog(&format!("[online] {}: joined \"{gname}\" ({n}/{})", self.who(s), js::number_to_string(max)), s);
        let gf = self.game_fields(g, vec![]);
        self.lsend(s, "gjoi", &gf, "");
        let w = self.user_fields(s);
        self.lsend(s, "+who", &w, "");
        let players: Vec<Option<SessId>> = self.lobby.game(g).map_or(vec![], |x| x.players.iter().map(|p| p.s).collect());
        for p in players.into_iter().flatten() {
            if p != s {
                let w = self.user_fields(s);
                self.lsend(p, "+who", &w, "");
            }
        }
        self.update(g);
    }

    /// a changed game to its players (+mgm) and the game list (+agm)
    fn update(&mut self, g: GameId) {
        let rec = encode_frame("+mgm", "", &self.game_fields(g, vec![])).expect("the lobby's own fields");
        let players: Vec<Option<SessId>> = self.lobby.game(g).map_or(vec![], |x| x.players.iter().map(|p| p.s).collect());
        for p in players.into_iter().flatten() {
            self.lsend_frame(p, &rec);
        }
        self.announce(g, false);
    }

    fn game_set(&mut self, s: SessId, f: &FieldMap) {
        let Some(g) = self.lobby.s(s).game.filter(|g| self.lobby.games.contains_key(g)) else {
            return self.lsend(s, "gset", &[], "ugam");
        };
        let mut changed = false;
        if f.has("USERFLAGS") {
            let flags = jsx::to_uint32(f.num("USERFLAGS", f64::NAN)) & self.title.flags.ready;
            let x = self.lobby.games.get_mut(&g).expect("live");
            match x.players.iter_mut().find(|p| p.s == Some(s)) {
                Some(p) => {
                    if p.flags != flags {
                        p.flags = flags;
                        changed = true;
                    }
                }
                // a session its game does not list: dropped as a server error, with the console line the Node server
                // wrote for it
                None => {
                    let line = format!(
                        "[online] {}: \"gset\" failed: TypeError: Cannot read properties of undefined (reading 'flags')",
                        self.who(s)
                    );
                    self.console_error(&line);
                    return self.ldrop(s, "server error");
                }
            }
        }
        let persona = self.lobby.s(s).persona.clone().unwrap_or_default();
        let (l, countdown) = (self.lobby.l, self.title.flags.countdown);
        let x = &self.lobby.games[&g];
        if persona == x.host_name && x.started == 0.0 {
            // the creator may change the game's settings
            let x = self.lobby.games.get_mut(&g).expect("live");
            if f.has("PARAMS") {
                let p = jsx::slice16(f.get("PARAMS").unwrap_or("null"), 256).to_string();
                if x.params != p {
                    x.params = p;
                    changed = true;
                }
            }
            if f.has("MAXSIZE") {
                let v = l.game_size.min((x.players.len() as f64).max(2.0).max(jsx::to_int32(f.num("MAXSIZE", f64::NAN)) as f64));
                if x.max_size != v {
                    x.max_size = v;
                    changed = true;
                }
            }
            if f.has("MINSIZE") {
                let v = x.max_size.min(2f64.max(jsx::to_int32(f.num("MINSIZE", f64::NAN)) as f64));
                if x.min_size != v {
                    x.min_size = v;
                    changed = true;
                }
            }
            // of its SYSFLAGS only the countdown: ranked, password and started stay the server's
            if f.has("SYSFLAGS") {
                let v = (x.sys_flags & !countdown) | (jsx::to_uint32(f.num("SYSFLAGS", f64::NAN)) & countdown);
                if x.sys_flags != v {
                    x.sys_flags = v;
                    changed = true;
                }
            }
            // one name (the Kick button) or several, comma-separated (the players not ready when the countdown ends)
            let kicks: IndexSet<String> = f.str("KICK").split(',').map(|n| lower(js::trim(n))).filter(|n| !n.is_empty()).collect();
            for kick in kicks {
                let Some(x) = self.lobby.game(g) else { break };
                let Some(k) = x.players.iter().find(|p| lower(&p.name) == kick && p.s != Some(s)).cloned() else { continue };
                let gname = x.name.clone();
                self.llog(&format!("[online] \"{gname}\": {} kicked out by {persona}", k.name), s);
                let Some(ks) = k.s else { continue };
                self.game_leave(ks, None);
                // its game learns it from the game's record that comes after its own record without the game: it
                // leaves the room (SPEED2.EXE 0x58dab0)
                let w = self.user_fields(ks);
                self.lsend(ks, "+who", &w, "");
                let gf = self.game_fields(g, vec![]);
                self.lsend(ks, "+mgm", &gf, "");
            }
        }
        let gf = self.game_fields(g, vec![]);
        self.lsend(s, "gset", &gf, "");
        // the others hear of a change only (a gset that changes nothing goes nowhere)
        if changed {
            self.update(g);
        }
    }

    fn game_start(&mut self, s: SessId) {
        let Some(g) = self.lobby.s(s).game.filter(|g| self.lobby.games.contains_key(g)) else {
            return self.lsend(s, "gsta", &[], "ugam");
        };
        let x = &self.lobby.games[&g];
        let persona = self.lobby.s(s).persona.clone().unwrap_or_default();
        if x.host_name != persona {
            return self.lsend(s, "gsta", &[], "host");
        }
        if (x.players.len() as f64) < x.min_size {
            return self.lsend(s, "gsta", &[], "nepl");
        }
        // one browser runs one game: players in the same browser are one player
        if x.players.iter().map(|p| p.vip).collect::<HashSet<_>>().len() != x.players.len() {
            return self.lsend(s, "gsta", &[], "nepl");
        }
        let (vip, group) = {
            let y = self.lobby.s(s);
            (y.vip, y.group.clone())
        };
        let (mv, mg) = self.lobby.games_of(vip, &group, true);
        if mv >= self.lobby.l.started_per_vip || mg >= self.lobby.l.started_per_group {
            return self.lsend(s, "gsta", &[], "full");
        }
        let now = self.now();
        let x = self.lobby.games.get_mut(&g).expect("live");
        x.started = now;
        x.sys_flags |= self.title.flags.active;
        // the race's id for the result reports: the same for every player, never empty
        let auth = Md5::digest(format!("{}:{}:{}:{}", x.id, x.name, js::number_to_string(now), rand::hex(8)).as_bytes());
        x.auth = jsx::hex(&auth);
        let (gname, names) = (x.name.clone(), x.players.iter().map(|p| p.name.clone()).collect::<Vec<_>>().join(", "));
        self.lobby.stats.races += 1;
        let seed = rand::int(1, 1 << 31);
        self.llog(&format!("[online] \"{gname}\": the race starts ({names})"), s);
        self.lsend(s, "gsta", &[], "");
        self.update(g);
        let players: Vec<(Option<SessId>, String)> =
            self.lobby.game(g).map_or(vec![], |x| x.players.iter().map(|p| (p.s, p.name.clone())).collect());
        let auth = self.lobby.game(g).map(|x| x.auth.clone()).unwrap_or_default();
        for (p, name) in players {
            let Some(p) = p else { continue };
            let gf = self.game_fields(g, fields![("AUTH", auth.clone()), ("SEED", seed), ("SELF", name)]);
            self.lsend(p, "+ses", &gf, "");
        }
    }

    fn game_delete(&mut self, s: SessId) {
        let Some(g) = self.lobby.s(s).game.filter(|g| self.lobby.games.contains_key(g)) else {
            return self.lsend(s, "gdel", &[], "ugam");
        };
        if self.lobby.games[&g].host_name != self.lobby.s(s).persona.clone().unwrap_or_default() {
            return self.lsend(s, "gdel", &[], "host");
        }
        self.lsend(s, "gdel", &[], "");
        self.end_game(g, "deleted by its creator");
    }

    /// s leaves its game: kind = the reply's kind ('glea': back to the game list), or None (the session or the game
    /// went)
    fn game_leave(&mut self, s: SessId, kind: Option<&str>) {
        let g = self.lobby.s(s).game.filter(|g| self.lobby.games.contains_key(g));
        let x = self.lobby.sm(s);
        x.game = None;
        if kind.is_some() {
            x.browsing = true;
        }
        let Some(g) = g else {
            if let Some(k) = kind {
                self.lsend(s, k, &[], "");
            }
            return;
        };
        if let Some(k) = kind {
            self.lsend(s, k, &[], "");
        }
        let persona = self.lobby.s(s).persona.clone();
        let Some(x) = self.lobby.game_mut(g) else { return };
        // a started race goes on without its players' lobby links (the game closes them at the start)
        if x.started != 0.0 {
            if let Some(p) = x.players.iter_mut().find(|p| p.s == Some(s)) {
                p.s = None;
            }
            return;
        }
        x.players.retain(|p| p.s != Some(s));
        if x.players.is_empty() || Some(&x.host_name) == persona.as_ref() {
            let why = if x.players.is_empty() { "empty" } else { "its creator left" };
            return self.end_game(g, why);
        }
        self.update(g);
    }

    fn end_game(&mut self, g: GameId, why: &str) {
        let Some(x) = self.lobby.games.remove(&g) else { return };
        let players: Vec<Option<SessId>> = x.players.iter().map(|p| p.s).collect();
        let group = x.group.clone();
        let gname = x.name.clone();
        self.lobby.dead.insert(g, x);
        for p in players.into_iter().flatten() {
            let Some(y) = self.lobby.sess.get_mut(&p) else { continue };
            if y.gone {
                continue;
            }
            if y.game == Some(g) {
                y.game = None;
                y.browsing = true;
            }
            let w = self.user_fields(p);
            self.lsend(p, "+who", &w, "");
            self.lsend(p, "+mgm", &fields![("IDENT", g)], "");
        }
        self.announce(g, true);
        (self.log)(&format!("[online] the game \"{gname}\" ended: {why} ({} games)", self.lobby.games.len()), Some(&group));
    }

    // ---- leaving, keep-alive ----

    pub fn lobby_leave(&mut self, s: SessId, why: &str) {
        let Some(x) = self.lobby.sess.get(&s) else { return };
        if x.gone {
            return;
        }
        if x.game.is_some() {
            self.game_leave(s, None);
        }
        self.lobby.sm(s).gone = true;
        self.lobby.remove_live(s);
        self.lobby.graveyard.push(s);
        self.leave_room(s);
        let x = self.lobby.s(s);
        if let Some(p) = &x.persona {
            let k = lower(p);
            if self.lobby.by_persona.get(&k) == Some(&s) {
                self.lobby.by_persona.remove(&k);
            }
        }
        if self.lobby.s(s).account.is_some() {
            self.llog(&format!("[online] {}: left ({why}; {} connected)", self.who(s), self.lobby.sessions.len()), s);
        }
    }

    /// the lobby's 5 s tick
    pub fn lobby_tick(&mut self) {
        self.forget_read_races();
        let now = self.now();
        let l = self.lobby.l;
        // what waits for its announce_ms (the timers do it; here too for a clock that is not the real one)
        let keys: Vec<LaterKey> =
            self.lobby.agm.keys().map(|g| LaterKey::Agm(*g)).chain(self.lobby.pop.keys().map(|r| LaterKey::Pop(*r))).collect();
        for k in keys {
            if self.lobby.later_mut(k).is_some_and(|a| a.timer.is_some() && now - a.at >= l.announce_ms) {
                self.fire(k);
            }
        }
        let sst = self.lobby.sst_counts();
        if self.lobby.last_sst != Some(sst) {
            self.lobby.last_sst = Some(sst);
            let f = encode_frame("+sst", "", &self.sst_fields()).expect("the lobby's own fields");
            let all: Vec<SessId> = self.lobby.sessions.iter().copied().collect();
            for s in all {
                if self.lobby.sessions.contains(&s) && self.lobby.s(s).persona.is_some() {
                    self.lsend_frame(s, &f);
                }
            }
        }
        let all: Vec<SessId> = self.lobby.sessions.iter().copied().collect();
        for s in all {
            let x = self.lobby.s(s);
            if now - x.last_in > l.idle_ms {
                let secs = jsx::round((now - x.last_in) / 1000.0);
                self.ldrop(s, &format!("silent for {} s", js::number_to_string(secs)));
                continue;
            }
            if x.persona.is_none() && now - x.t0 > l.pre_login_ms {
                self.ldrop(s, "no persona chosen");
                continue;
            }
            if now - x.last_ping >= l.ping_ms {
                self.lobby.sm(s).last_ping = now;
                self.lsend(s, "~png", &fields![("REF", day(now))], "");
            }
        }
        let away = |gv: &HashMap<u32, f64>, vip: u32| now - gv.get(&vip).copied().unwrap_or(f64::INFINITY) > l.orphan_ms;
        let ids: Vec<GameId> = self.lobby.games.keys().copied().collect();
        for g in ids {
            let Some(x) = self.lobby.games.get(&g) else { continue };
            if x.started == 0.0 {
                continue;
            }
            if now - x.started > l.race_ms {
                self.end_game(g, "the race is long over");
            } else if x.players.iter().all(|p| away(&self.lobby.gone_vips, p.vip)) {
                self.end_game(g, "its players left the server");
            }
        }
        self.lobby.gone_vips.retain(|_, t| now - *t <= l.race_ms);
    }

    /// the server stops: the timers end, every session's link goes, the races going on and the results are written
    pub fn lobby_stop(&mut self) {
        // (the timers end, but each record keeps its timer's id: a `later` after this sends nothing)
        let keys: Vec<LaterKey> =
            self.lobby.agm.keys().map(|g| LaterKey::Agm(*g)).chain(self.lobby.pop.keys().map(|r| LaterKey::Pop(*r))).collect();
        for k in keys {
            if let Some(t) = self.lobby.later_mut(k).and_then(|a| a.timer) {
                self.timers.clear(t);
            }
        }
        let all: Vec<SessId> = self.lobby.sessions.iter().copied().collect();
        for s in all {
            let conn = self.lobby.s(s).conn;
            self.conn_destroy(conn, "server stopping");
        }
        self.save_races();
        self.stats.stop();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fit_and_times() {
        assert_eq!(fmt_time(186.82000732421875), "3:06.82");
        assert_eq!(fmt_time(11.199999809265137), "0:11.20");
        assert_eq!(fmt_time(5.0), "0:05.00");
        let f = fit(fields![("A", "x".repeat(10)), ("B", "y")], 6, &["A"]);
        assert_eq!(f.len(), 1);
        assert!(four_printable("auth") && four_printable("\\x80") && !four_printable("ab") && !four_printable("a b "));
    }
}
