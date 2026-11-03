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
//!   takes it back), and a claimed name only for the keyphrase that claimed it (nfsu2-claims; the owner the page's
//!   hello named, `FrameCtx::owner`), whose session takes it from anyone else's: a name another player holds or
//!   claimed is given with four digits in place of its end, and the game takes it for its own (runtime/online.c)
//! - move -> into a room (+who, +usr, +pop), or out (an unknown NAME)
//! - mesg -> chat in the room, or to one persona (PRIV; the game's challenge invites are private messages too): +msg.
//!   Not to a persona that blocks the sender (EA Messenger's blocked list, buddy.rs).  A line that reads as a slur
//!   (nfsu2-names `chat_slur`: the name lists, a word at a time) goes back to its sender only, who sees it as said;
//!   `slur_lines` of them in `slur_window_ms` mute the sender for `slur_mute_ms`, and the log says so
//! - rept -> "Submit Complaint" (a player's options in a game room: "Another player is constantly using obscene or
//!   vulgar language in chat rooms."): the log line names that player (its virtual address, for the operator's
//!   `ctl ban`) and its last `said_lines` lines of the last `said_ms` that the complainer could hear (in a room or
//!   game, or private to the complainer), kept in memory for that only; the lobby does nothing else about it
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
//! who finished it, in different browsers, reported the same places for the racers who crossed the finish line, no
//! sooner than the race type's shortest race; a ranked race needs three players for its points.
//!
//! Sessions and games are kept by id.  A session that left (and a game that ended) stays readable until the event
//! that ended it is over (the graveyard), so the code still handling that event goes on with it: a send to a gone
//! session does nothing, and the others may still hear of it.

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet, VecDeque};
use std::sync::Arc;

use hmac::{Hmac, Mac};
use indexmap::{IndexMap, IndexSet};
use md5::Md5;
use nfsu2_util::{js, rand, vip_str};
use sha2::{Digest, Sha256};

use crate::aries::{FieldMap, Fields, FrameReader, MAX_FRAME, V, encode_fields, encode_frame, is_secret};
use crate::core::Core;
use crate::jsx::{self, clean_text, day, lower, valid_name};
use crate::stats::{OUT, Period, RACE_TYPES, Race, RacePlayer, RaceReport, ReportPlayer, decode_result, race_points};
use crate::timers::{Action, TimerId};
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
    /// lines that read as a slur within slur_window_ms: the sender is muted for slur_mute_ms
    pub slur_lines: f64,
    pub slur_window_ms: f64,
    pub slur_mute_ms: f64,
    /// a persona's last lines kept for a complaint about it, for at most said_ms
    pub said_lines: f64,
    pub said_ms: f64,
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
            sessions: 7000.0,
            per_vip: 3.0,
            per_group: 8.0,
            per_site: 32.0,
            free_share: 0.25,
            msg_rate: 20.0,
            chat_rate: 1.0,
            chat_len: 255.0,
            mute_ms: 10_000.0,
            slur_lines: 3.0,
            slur_window_ms: 600_000.0,
            slur_mute_ms: 600_000.0,
            said_lines: 5.0,
            said_ms: 600_000.0,
            create_rate: 0.5,
            act_rate: 1.0,
            announce_ms: 1000.0,
            personas: 4.0,
            games: 2100.0,
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

/// the rooms everyone shares: a lounge and one per race type
pub const ROOMS: [&str; 7] = ["Lounge", "Circuit", "Sprint", "Drift", "Drag", "StreetX", "URL"];
const ROOM_LIMIT: usize = 50;

const TIER: &str = "0,1999,4999,9999,19999,39999,59999,79999,99999,119999";

/// games in one game list search: the game's page (its search asks for this many, START a page after another).  The
/// lobby shows one search's games until the next (the +agm updates never reach its rows), and a step past the last
/// row of a full page searches the next page (of a shorter list: the selection stays); at the game's own default, 20
/// (SPEED2.EXE 0x4bb14b), this server's list came in pages of 20 from searches seconds apart, which skipped and
/// repeated games and replaced the rows under the player ("Race lobby list is moving too fast", a player's report,
/// 2026-10-25).  This many: every open game in one search.
const GAMES_PER_UPDATE: usize = 200;

/// the server config (the "news" reply, code new7) the game asks for at connect: where the buddy server is (here:
/// this same host), the news and terms pages, the tier points per race type, the game list's page
pub fn config_fields() -> Fields {
    fields![
        ("TOSURL", "http://10.77.0.1/tos"),
        ("NEWSURL", "http://10.77.0.1/news"),
        ("BUDDY_SERVER", "10.77.0.1"),
        ("BUDDY_PORT", PORT_BUDDY),
        ("GAMES_PER_UPDATE", GAMES_PER_UPDATE),
        ("CIRCUIT_TIER_POINTS", TIER),
        ("SPRINT_TIER_POINTS", TIER),
        ("DRIFT_TIER_POINTS", TIER),
        ("DRAG_TIER_POINTS", TIER),
        ("STREET_CROSS_TIER_POINTS", TIER),
        ("URL_TIER_POINTS", TIER),
    ]
}

/// OPFLAG bit of a player who is ready (gset USERFLAGS, server.dll)
const READY: u32 = 1 << 27;
/// SYSFLAGS bits of a game with a password and of a started one.  The password bit is the server's: the game's gcre
/// never sets it (only RANKED, 0x58d580), and its game list asks for the password of a game that has it before it
/// joins (0x4f1713); a wrong one is "pass" (0x49ebc3, 0x55911e)
const PASSWORD: u32 = 1 << 16;
const ACTIVE: u32 = 1 << 19;
/// SYSFLAGS bit of a ranked game (Options: Game Type Ranked; the game then asks for 3 players at least, and its
/// lobby's default search asks for unranked games: SYSMASK bits 18 and 19)
const RANKED: u32 = 1 << 18;
/// SYSFLAGS bit of a game counting down to its start (the creator's gset; server.dll takes the creator's SYSFLAGS):
/// Start Race while some players are ready and others not sets it (SPEED2.EXE 0x58aa20), every game in the room then
/// counts down 10 s (0x4d2852, 0x4e5982), and the creator's game kicks the players still not ready (gset KICK, a
/// list) and starts (0x58a7e0); a start that fails clears it (0x58ab40)
const COUNTDOWN: u32 = 1 << 12;
/// CUSTFLAGS bit of a private game (Options: Private Room, SPEED2.EXE 0x58d55d; the game's own search always asks
/// for it clear, 0x590669: its players come by invitation)
const PRIVATE: u32 = 1 << 8;
/// a search's code (the header's [4..7], 0 in the game's own requests) when the player left the game's Filter screen
/// with the settings it asks for (runtime/olfilter.c)
const FILTER_SET: &str = "filt";
/// the Filter's defaults as a search asks for them (Circuit, no car restriction, Collision Detection and Race Breaker
/// on, unranked, not private): CUSTFLAGS, CUSTMASK, SYSFLAGS, SYSMASK
const FILTER_DEFAULT: [u32; 4] = [0x40000f3, 0x40001f3, 0, 0xc0000];
/// the race types a report may name: RACE_TYPES, which have their stat blocks, and the game's seventh (a game with
/// CUSTFLAGS bit 25, which SPEED2.EXE 0x5fe8d0 reports as 6), which has no stat block or tier points: its results
/// count nowhere
const REPORT_TYPES: [&str; 7] = ["circuit", "sprint", "drag", "drift", "streetx", "url", "type 6"];
/// the shortest race of each type (REPORT_TYPES order) a result may come from, in seconds: well under the game's own
/// shortest (one lap of the shortest track; a quarter-mile drag)
const MIN_RACE_S: [f64; 7] = [20.0, 20.0, 5.0, 20.0, 20.0, 20.0, 20.0];
/// A race's winner in under this share of the next finisher's time is a `[cheat]` line (`far_ahead`): a teleport to
/// the finish, a speed hack.  Real races are far closer (a car twice as slow is lapped in a circuit race long before
/// it finishes); a drift race is won by points, and is left out.
const FAR_AHEAD: f64 = 0.4;
const KIND_DRIFT: u8 = 3;
/// A race's winner in under this share of the third quickest other racer's time of its track (race type, track,
/// direction and laps: each finisher's of its clean races, two finishers or more and no line of the racer), once the
/// track had QUICK_WINS clean races, is a `[cheat]` line (`quick_win`): a car made faster than any tuning makes one, a
/// speed hack, wins far quicker than the best honest racers do (the free-roam lobbies' races of 2026-10-30 on
/// nfsu2.xyz: one player's sprint in 0.58 of it; of 4,727 other wins the quickest 0.78).  Not when another finisher of
/// the race came close (the winner over THIRD_SHARE of its time): that time can be driven, and the track's best
/// winners were slower cars (nfsu2.xyz, 2026-10-31: four racers within 4 s of each other, all under 70 % of the third
/// quickest of 10 clean wins).  Every finisher counts, not the winners alone: the quick cars' races are often close,
/// one of them the winner (nfsu2.xyz, 2026-10-31: a drag racer won two drags in 23.3 s, under 70 % of the third
/// quickest of 12 winners' 36.9 s, having finished within 0.6 s of two others in 23.4 to 24.0 s in another).  The
/// tracks kept (`Lobby::wins`), and the quickest racers each.
const THIRD_SHARE: f64 = 0.7;
const QUICK_WINS: u32 = 10;
const TRACKS_KEPT: usize = 4096;
const TOP_KEPT: usize = 4;
/// no car of the game averages this over a race (m/s: 350 km/h; the fastest tuned cars make 350 at the most)
const RACE_MPS: f64 = 350.0 / 3.6;
/// a finisher who drove less than this share of what another racer drove (`race_cheats`)
const DROVE_SHARE: f64 = 0.5;
/// a racer still on the way when the race ended: its way counts against a finisher's when its time is at most this
/// much longer (s; the race ends 30 s after the first finished).  A game that went on far longer was cut off from the
/// others' race: nfsu2.xyz, 2026-10-31, one went on alone for 12 minutes, drove 19.6 km and made the race's 2:54
/// winner one who drove "under half"
const ENDED_S: f64 = 40.0;
/// A racer's own figures are taken (`race_cheats`: its time, its average, the way it drove) once another racer's game
/// showed it finish within AGREE_S of its own time (the games send each other their finish times: their reports agree
/// to a tenth), and only when no other finisher came within CLOSE_SHARE of it.  nfsu2.xyz, 2026-10-31: two drag
/// winners' own games had their times 7 s short (0:17.35 to the other racer's game's 0:24.14) and so 357 and 500 km/h
/// on average; one circuit winner's own game had it at 388 km/h on average with a top speed of 183, the second 0.15 s
/// behind; one counted only the first lap of two (3 km in 1:22, the other racer's game saw 2:35): all honest racers,
/// in close races all day.  A cheat's own figures are the others' too (a racewin's 4.77 s, a teleport's), and nobody
/// finishes near it.
const AGREE_S: f64 = 1.0;
const CLOSE_SHARE: f64 = 0.95;
/// the finish times kept of what the other racers' games showed of a racer (`Player::seen`)
const SEEN_KEPT: usize = 8;
/// a top speed no car of the game makes, however tuned (m/s: 420 km/h), an edited save's (`race_cheats`): of the
/// 38,967 personas with a top speed record on nfsu2.xyz on 2026-10-30, half had under 218 km/h, the fastest tuned
/// cars 377, 12 over 380 and 5 over 420 (one the racer of a circuit won at 383 km/h on average); a free-roam lobby
/// race's edited 350Z made 444
const CAR_MPS: f64 = 420.0 / 3.6;
/// the Rankings screens' six boards in their order (Circuit, Sprint, Drift, Drag, Street X, URL: SPEED2.EXE 0x58ce30,
/// 0x582530) as race types (RACE_TYPES: drag 2, drift 3)
const SLOT_TYPES: [u32; 6] = [0, 1, 3, 2, 4, 5];
/// what a ranked race's results screen counts down from until its table comes, in ms ("Waiting 60 seconds for
/// results", then "Waiting for final server results"; SPEED2.EXE 0x4e9210): a guess at how long the others take to
/// finish and report, no limit (the table comes once the result stands)
const RESULTS_WAIT_MS: f64 = 60_000.0;

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
    /// the keyphrase its page plays with (nfsu2-claims' owner), if it named one
    pub owner: Option<Arc<str>>,
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
    /// when its lines that read as a slur were said (those of the last slur_window_ms)
    slurs: Vec<f64>,
    pub gone: bool,
}

/// a line a persona said, kept for a complaint about it
#[derive(Clone, Debug)]
pub struct Said {
    pub t: f64,
    pub text: String,
    /// the persona (lower case) a private line went to
    pub to: Option<String>,
    /// it read as a slur: only its sender saw it
    pub hidden: bool,
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
    /// what its own game measured of it in the race (its own entry in its first report of the right shape)
    pub own: Option<Own>,
    /// the finish times the other racers' games showed of it, in their reports of the right shape (SEEN_KEPT at most)
    pub seen: Vec<f64>,
}

/// A racer's own figures (`race_cheats`): finished, its place, time (s), average and top speed (m/s), a drift race's
/// points, the car type its game says it raced with edited data (`edited_code`), and whether a `[cheat]` line told of it
#[derive(Clone, Copy, Debug)]
pub struct Own {
    pub finished: bool,
    pub place: u8,
    pub time: f64,
    pub score: i32,
    pub avg: f64,
    pub top: f64,
    pub edited: Option<u8>,
    pub told: bool,
}

/// the game's player car types (CarTypeInfo 0..30 in GLOBAL\GlobalB), for `edited_code`'s
const CAR_TYPES: [&str; 31] = [
    "PEUGOT", "FOCUS", "COROLLA", "240SX", "MIATA", "CIVIC", "PEUGOT106", "CORSA", "HUMMER", "NAVIGATOR", "ESCALADE", "TIBURON",
    "SENTRA", "CELICA", "IS300", "SUPRA", "GOLF", "A3", "RSX", "ECLIPSE", "TT", "RX8", "350Z", "G35", "3000GT", "GTO", "MUSTANGGT",
    "SKYLINE", "LANCEREVO8", "RX7", "IMPREZAWRX",
];

/// A rank's code from a game that raced a car whose data is edited (runtime/mprace.c mprace_rank_code: the car type's
/// record in its copy of GLOBAL\GlobalB is not the game's; a guide passed among players, 2026-10-31, has an AI agent
/// make one car 3,600 kW with x2.5 grip and brakes in the browser's copy): "ed" and the type in two hex digits, "edff"
/// when the game had no car to tell (0xff here).  None for any other code (the game's own: 0)
fn edited_code(code: &str) -> Option<u8> {
    let hex = code.strip_prefix("ed").filter(|h| h.len() == 2)?;
    u8::from_str_radix(hex, 16).ok()
}

/// "SUPRA, type 15" for `edited_code`'s type; "type unknown" when the game could not tell
fn edited_car(t: u8) -> String {
    CAR_TYPES.get(t as usize).map_or("type unknown".to_string(), |n| format!("{n}, type {t}"))
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
    /// lower-case persona -> its last lines (said_lines of the last said_ms), the oldest first; a persona's lines
    /// stay after it left
    pub said: HashMap<String, VecDeque<Said>>,
    /// the clean races of each track (race type, track, direction, laps) since the start (`quick_win`)
    pub wins: HashMap<(u8, u16, u8, u8), Wins>,
}

/// A track's clean races: how many, and the quickest finishers (each once by lower-case name, its best, quickest first)
#[derive(Clone, Debug, Default)]
pub struct Wins {
    pub wins: u32,
    pub top: Vec<(String, f64)>,
}

impl Wins {
    /// one finisher's time (a race counts in `wins` by itself)
    fn add(&mut self, who: String, time: f64) {
        match self.top.iter_mut().find(|(w, _)| *w == who) {
            Some(x) => x.1 = x.1.min(time),
            None => self.top.push((who, time)),
        }
        self.top.sort_by(|a, b| a.1.total_cmp(&b.1));
        self.top.truncate(TOP_KEPT);
    }

    /// the third quickest racer's time but `who`'s, once there were QUICK_WINS races
    fn third(&self, who: &str) -> Option<f64> {
        (self.wins >= QUICK_WINS).then(|| self.top.iter().filter(|(w, _)| w != who).nth(2).map(|x| x.1)).flatten()
    }
}

impl Lobby {
    pub fn new(l: Limits) -> Lobby {
        Lobby {
            l,
            sessions: BTreeSet::new(),
            sess: HashMap::new(),
            by_vip: HashMap::new(),
            by_group: HashMap::new(),
            by_site: HashMap::new(),
            graveyard: Vec::new(),
            by_persona: HashMap::new(),
            rooms: ROOMS.iter().enumerate().map(|(i, n)| Room { id: i as u32 + 1, name: n, users: IndexSet::new() }).collect(),
            games: BTreeMap::new(),
            dead: HashMap::new(),
            agm: IndexMap::new(),
            pop: IndexMap::new(),
            gone_vips: HashMap::new(),
            next_user: 1,
            next_game: 1,
            stats: LobbyStats::default(),
            last_sst: None,
            said: HashMap::new(),
            wins: HashMap::new(),
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

/// The `[cheat]` lines of a race so far, at the report of game player `me` (`mine`: its own entry in it), by what
/// each racer's own game measured of it (`Own`): what a game shows of the other cars is what came over the network,
/// and of a racer who left the race, a car put far away (nfsu2.xyz, 2026-10-29: one game had another racer drive
/// 462 km in a 4-minute sprint; 18 lines of a morning came of such figures, each about a racer other than the
/// reporter, none of them a cheat).  One line a racer a race, for a finisher whose own game says it did what no racer
/// does: a time under its race type's shortest (MIN_RACE_S: the report is not taken either), RACE_MPS or more on
/// average, or a way (its average times its time) under DROVE_SHARE of another racer's own, finished or still on the
/// way when the race ended (ENDED_S) (a cheat that ends its race at once: a test's racewin won a sprint in 4.77 s
/// having driven 65 m, the other racer 1021 m by the race's end 30 s later; an honest finisher drives the whole way,
/// and nobody twice as much before the race ends).  The time, the average and the way count only once another racer's
/// game showed the racer finish at its own time (AGREE_S), and not when another finisher came close (CLOSE_SHARE): an
/// honest game's own figures may be wrong.  `views`: the finishers' times as this report shows them (game player, s),
/// its reporter's own left out.  The way is looked at again at every report: the other racer's may come after the
/// finisher's.  A drift race is won by points: only its top speed is looked at.  The line names
/// the race's track and laps and the racer's top speed: the other finishers' times are on the race's `[online]` lines
/// (often none yet: one who won in 30 s had the others give up).
fn race_cheats(
    game: &str,
    r: &RaceReport,
    me: usize,
    mine: Option<&ReportPlayer>,
    edited: Option<u8>,
    views: &[(usize, f64)],
    players: &mut [Player],
) -> Vec<String> {
    let kind = r.kind;
    let drift = kind == KIND_DRIFT;
    if let Some(p) = mine
        && players[me].own.is_none()
    {
        let speed = |v: f64| if v < 1000.0 { v.max(0.0) } else { 0.0 };
        let time = if p.time < 100_000.0 { p.time.max(0.0) } else { 0.0 };
        let finished = !p.dropped && p.place >= 1 && time > 0.0;
        let score = p.score.max(0);
        players[me].own = Some(Own { finished, place: p.place, time, score, avg: speed(p.avg), top: speed(p.top), edited, told: false });
    }
    for &(j, t) in views {
        if j != me && players[j].seen.len() < SEEN_KEPT {
            players[j].seen.push(t);
        }
    }
    let ty = REPORT_TYPES.get(kind as usize).copied().unwrap_or("race");
    let laps = if r.laps > 0 { format!(", {} lap{}", r.laps, if r.laps == 1 { "" } else { "s" }) } else { String::new() };
    let mut lines = Vec::new();
    for i in 0..players.len() {
        let Some(o) = players[i].own.filter(|o| !o.told) else { continue };
        let drove = o.avg * o.time;
        let mut why = Vec::new();
        if !o.finished && o.top < CAR_MPS && o.edited.is_none() {
            continue;
        }
        // its own time, average and way: a finisher's that another game saw finish then, no other finisher close
        let agreed = players[i].seen.iter().any(|&t| (t - o.time).abs() <= AGREE_S);
        let close = players.iter().enumerate().any(|(j, x)| {
            j != i && x.own.filter(|y| y.finished).map(|y| y.time).into_iter().chain(x.seen.iter().copied()).any(|t| t * CLOSE_SHARE <= o.time)
        });
        let taken = !drift && o.finished && agreed && !close;
        if let Some(&min) = MIN_RACE_S.get(kind as usize)
            && taken
            && o.time < min
        {
            why.push(format!("under the {} s a {ty} race takes at least", js::number_to_string(min)));
        }
        if taken && o.avg >= RACE_MPS {
            why.push(format!("{:.0} km/h on average, which no car does", o.avg * 3.6));
        }
        let most = players
            .iter()
            .enumerate()
            .filter_map(|(j, x)| Some((x.own.filter(|y| j != i && (y.finished || y.time <= o.time + ENDED_S))?, &x.name)))
            .map(|(x, name)| (x.avg * x.time, name))
            .max_by(|a, b| a.0.total_cmp(&b.0));
        let each = if let Some((d, other)) = most
            && taken
            && drove < DROVE_SHARE * d
        {
            why.push(format!("having driven {drove:.0} m, under half the {d:.0} m \"{other}\" drove"));
            "each "
        } else {
            ""
        };
        if let Some(t) = o.edited {
            why.push(format!("an edited car ({}: its game has the car's data edited, GlobalB)", edited_car(t)));
        }
        if o.top >= CAR_MPS {
            why.push(format!("an edited car (no car of the game makes {:.0} km/h)", CAR_MPS * 3.6));
        }
        if why.is_empty() {
            continue;
        }
        let p = &mut players[i];
        p.own = Some(Own { told: true, ..o });
        let place = match (o.finished, o.place) {
            (false, _) => "raced in".to_string(),
            (true, 1) => "won".to_string(),
            (true, n) => format!("finished {n} in"),
        };
        let time = match (o.finished, drift) {
            (true, false) => format!(" in {}", fmt_time(o.time)),
            (true, true) => format!(" with {} points", o.score),
            (false, _) => String::new(),
        };
        lines.push(format!(
            "[cheat] {} \"{}\": {place} the online race \"{game}\" ({ty}, track {}{laps}){time}, {}: {each}by its own game; \
             its top speed {:.0} km/h",
            vip_str(p.vip),
            p.name,
            r.track,
            why.join(", "),
            o.top * 3.6
        ));
    }
    lines
}

/// The `[cheat]` line of a race whose winner (the quickest finisher) took under FAR_AHEAD of the next finisher's time,
/// with the winner's address for `nfsu2-server ctl kick|ban`; None for any other race
fn far_ahead(game: &str, kind: u8, order: &[RacePlayer], players: &[Player]) -> Option<String> {
    if kind == KIND_DRIFT {
        return None;
    }
    let mut done: Vec<&RacePlayer> = order.iter().filter(|p| p.place != 0 && p.time > 0.0).collect();
    done.sort_by(|a, b| a.time.total_cmp(&b.time));
    let [a, b, ..] = done[..] else { return None };
    if a.time >= FAR_AHEAD * b.time {
        return None;
    }
    let vip = players.iter().find(|p| lower(&p.name) == lower(&a.name)).map_or(0, |p| p.vip);
    Some(format!(
        "[cheat] {} \"{}\": won the online race \"{game}\" ({}) in {}, under {:.0} % of the next finisher's {} (\"{}\")",
        vip_str(vip),
        a.name,
        REPORT_TYPES.get(kind as usize).unwrap_or(&"race"),
        fmt_time(a.time),
        FAR_AHEAD * 100.0,
        fmt_time(b.time),
        b.name
    ))
}

/// The `[cheat]` line of a race whose result stands, when its winner took under THIRD_SHARE of the third quickest other
/// racer's time of its track, and no other finisher came close (THIRD_SHARE); else None, and a clean race (two
/// finishers or more, its winner told of by no line, `told`) counts in the track's, with each finisher's time but those
/// a line told of
fn quick_win(
    wins: &mut HashMap<(u8, u16, u8, u8), Wins>,
    game: &str,
    r: &RaceReport,
    order: &[RacePlayer],
    players: &[Player],
    told: bool,
) -> Option<String> {
    if r.kind == KIND_DRIFT {
        return None;
    }
    let w = order.iter().find(|p| p.place == 1 && p.time > 0.0)?;
    let key = (r.kind, r.track, r.dir, r.laps);
    let who = lower(&w.name);
    let close = order.iter().any(|p| p.place > 1 && p.time > 0.0 && w.time >= THIRD_SHARE * p.time);
    if !close && let Some(t) = wins.get(&key).and_then(|x| Some((x.third(&who)?, x.wins))).filter(|&(t, _)| w.time < THIRD_SHARE * t) {
        let vip = players.iter().find(|p| lower(&p.name) == who).map_or(0, |p| p.vip);
        let ty = REPORT_TYPES.get(r.kind as usize).copied().unwrap_or("race");
        let laps = if r.laps > 0 { format!(", {} lap{}", r.laps, if r.laps == 1 { "" } else { "s" }) } else { String::new() };
        return Some(format!(
            "[cheat] {} \"{}\": won the online race \"{game}\" ({ty}, track {}{laps}) in {}, under {:.0} % of the third \
             quickest other racer's time of the track's {} clean races ({}): as the result stands",
            vip_str(vip),
            w.name,
            r.track,
            fmt_time(w.time),
            THIRD_SHARE * 100.0,
            t.1,
            fmt_time(t.0)
        ));
    }
    let finished = order.iter().filter(|p| p.place != 0 && p.time > 0.0).count();
    if finished >= 2 && !told && (wins.len() < TRACKS_KEPT || wins.contains_key(&key)) {
        let track = wins.entry(key).or_default();
        track.wins += 1;
        let told_of = |name: &str| players.iter().any(|p| lower(&p.name) == lower(name) && p.own.is_some_and(|o| o.told));
        for p in order.iter().filter(|p| p.place != 0 && p.time > 0.0 && !told_of(&p.name)) {
            track.add(lower(&p.name), p.time);
        }
    }
    None
}

/// What decides a race's result in a kept report ("Ann:1,Bob:2|Cat,Dan", `Core::rank`): the racers who crossed the
/// finish line at their places.  A report kept from before 2026-11 named every racer, "Cat:0" for one who did not
/// finish: it reads the same.
fn finishers_of(key: &str) -> String {
    let done = key.split('|').next().unwrap_or("");
    done.split(',').filter(|e| !e.is_empty() && !e.ends_with(":0")).collect::<Vec<_>>().join(",")
}

/// the racers a kept report names who did not finish (after '|'; before 2026-11 "name:0")
fn others_of(key: &str) -> Vec<&str> {
    let (done, rest) = key.split_once('|').unwrap_or((key, ""));
    let old = done.split(',').filter_map(|e| e.strip_suffix(":0"));
    old.chain(rest.split(',')).filter(|e| !e.is_empty()).collect()
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
        let owner = self.conn_owner(c);
        self.lobby.sess.insert(
            id,
            Session {
                id,
                conn: c,
                vip,
                group,
                site,
                owner,
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
                slurs: Vec::new(),
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
                    self.lsend(s, "news", &config_fields(), "new7")
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
            "rank" => self.rank(s, f, code),
            "snap" => self.snap(s, f),
            "rept" => self.complaint(s, f),
            _ => self.lsend(s, kind, &[], "badc"),
        }
    }

    // ---- login ----

    fn auth(&mut self, s: SessId, kind: &str, f: &FieldMap) {
        let name = f.str("NAME").to_string();
        if self.lobby.s(s).account.is_some() {
            return self.lsend(s, kind, &[], "logn");
        }
        // a name that reads as a slur (nfsu2-names), or the operator took away, as one the game would not let one type
        if !valid_name(&name) || nfsu2_names::blocked_name(&name) || self.denied(&name) {
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
        if !valid_name(&p) || nfsu2_names::blocked_name(&p) || self.denied(&p) {
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
        let claimed = !self.may_claim(&mine, s);
        let mine = if claimed || self.name_taken(&mine, s) {
            match self.free_variant(&mine, s) {
                Some(v) => v,
                None => return self.lsend(s, "pers", &[], "pset"),
            }
        } else {
            mine
        };
        // a claim holds HOLD_DAYS (two years) from its owner's last game
        if let (Some(c), Some(o)) = (&self.claims, &self.lobby.s(s).owner) {
            c.seen(&mine, o, self.now() as i64);
        }
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
        // (its Messenger lists in memory while it plays: the chat hides the lines of the players it blocks)
        self.buddy_fetch(&lower(&mine));
        let taken = match (mine != asked, claimed) {
            (false, _) => String::new(),
            (true, false) => format!(" (\"{asked}\" is taken)"),
            (true, true) => format!(" (\"{asked}\" is claimed)"),
        };
        self.llog(&format!("[online] {addr}: plays as \"{mine}\"{taken}"), s);
        let r = self.pers_fields(s, &mine);
        self.lsend(s, "pers", &r, "");
        let w = self.user_fields(s);
        self.lsend(s, "+who", &w, "");
        let sst = self.sst_fields();
        self.lsend(s, "+sst", &sst, "");
    }

    /// name's holder gives way to s when it is s's browser's own older session (its game logged in again) or its
    /// game is gone (a stale session), and to the keyphrase that claimed the name (nfsu2-claims): a holder that is not
    /// its owner leaves the lobby (its game logs in again under the name with digits)
    fn give_way(&mut self, name: &str, s: SessId) {
        let Some(h) = self.lobby.by_persona.get(&lower(name)).copied() else { return };
        if h == s {
            return;
        }
        let Some(hx) = self.lobby.sess.get(&h) else { return };
        let owner = self.lobby.s(s).owner.clone();
        let by_owner = owner.is_some() && hx.owner != owner && self.may_claim(name, s) && !self.may_claim_as(name, hx.owner.as_deref());
        if hx.gone || hx.vip == self.lobby.s(s).vip || self.now() - hx.last_in >= 30_000.0 {
            self.ldrop(h, "the persona logged in again");
        } else if by_owner {
            self.ldrop(h, "the name's owner logged in");
        }
    }

    /// the operator took name away (nfsu2-bans `rename`: it reads as one of the names taken away)
    fn denied(&self, name: &str) -> bool {
        self.deny.as_ref().is_some_and(|d| d.has(name))
    }

    /// s's player races nobody (the operator's race ban: its connection's last frame said so, FrameCtx::no_races)
    fn races_off(&self, s: SessId) -> bool {
        self.vh.conns.get(&self.lobby.s(s).conn).is_some_and(|c| c.no_races)
    }

    /// s may play under name as far as the claims go: nobody claimed it, or s's keyphrase did
    fn may_claim(&self, name: &str, s: SessId) -> bool {
        self.may_claim_as(name, self.lobby.s(s).owner.as_deref())
    }

    fn may_claim_as(&self, name: &str, owner: Option<&str>) -> bool {
        self.claims.as_ref().is_none_or(|c| c.may_use(name, owner, self.now() as i64))
    }

    /// name is another player's: a session that holds it, or a player racing under it (a race this server started,
    /// the player not back yet) from another browser that is still on the switch, who comes back to it
    fn name_taken(&self, name: &str, s: SessId) -> bool {
        if !self.may_claim(name, s) {
            return true;
        }
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
            let v = if nfsu2_names::blocked_name(&cut) || self.denied(&cut) { format!("Player{d}") } else { cut };
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
        fields![("I", r.id), ("N", r.name), ("H", "NFSU2"), ("F", "CK"), ("T", r.users.len()), ("L", ROOM_LIMIT), ("P", 0)]
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
                ("SERV", "NFSU2"),
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
    /// places for the racers who crossed the finish line, `ReportPlayer::finished`): one player alone cannot decide
    /// it; later reports are compared with it.  The others did not finish, and are not compared: a racer who left may
    /// be gone in one game's report and left out of another's.  The reports that decide it are those of the players
    /// who raced to the end: who finished, and who was still on the way when the race ended, 30 s after the first
    /// finisher (`stats::OUT`: its game saw the finishers cross the line).  Until 2026-11 only the finishers' did, and
    /// the racers on the way were out like leavers: a race that one racer finished and the others ran out of time in
    /// (a test race, 2026-11-02: "did not finish: the report decides nothing" twice) never counted, nor showed
    /// "Official Race Results".  The game of a player who left the race (dropped out, quit) reports every player as
    /// dropped and is back first, so two of them decided a fifth of the live results as "everyone DNF" before the
    /// finishers reported (2026-10-24); such a report decides nothing, is logged, and its player sees the table.  The
    /// result is the report that decided it: the finishers at their places, the others DNF (a disconnect in the
    /// career only for one who left).  A ranked race of fewer than three players (the game's own minimum) counts as
    /// unranked.  The game is told "Unranked" either way (what the game shows after the race); a ranked game's players
    /// see its table on their results screens (`results_table`).
    fn rank(&mut self, s: SessId, f: &FieldMap, code: &str) {
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
        let r = decode_result(f.get("RESU").unwrap_or("undefined"));
        let mut names = Vec::new();
        let mut i = 0;
        while f.has(&format!("NAME{i}")) && i < 8 {
            names.push(f.get(&format!("NAME{i}")).unwrap_or("null").to_string());
            i += 1;
        }
        let mut why = self.check_report(g, r.as_ref(), &names);
        let spers = tpl(&self.lobby.s(s).persona).to_string();
        let gname = self.lobby.game(g).expect("held").name.clone();
        // what the racers' own games measured that no racer does, whether this report is taken or not (a time too
        // short for the race is not); a report of the wrong shape says nothing of anyone (one read out of line named
        // one racer twice, with times of 0.2 s, 2026-10-29)
        if let Some(r) = &r
            && why.is_empty()
        {
            let mine = r.players.iter().find(|p| lower(&names[p.index as usize]) == persona);
            let players = &mut self.lobby.game_mut(g).expect("held").players;
            // (the finishers' times as this report shows them, by game player: what this game saw of the others)
            let views: Vec<(usize, f64)> = r
                .players
                .iter()
                .filter(|p| !p.dropped && p.place >= 1 && p.time > 0.0 && p.time < 100_000.0)
                .filter_map(|p| {
                    let want = lower(&names[p.index as usize]);
                    Some((players.iter().position(|x| lower(&x.name) == want)?, p.time))
                })
                .collect();
            let lines = race_cheats(&gname, r, me, mine, edited_code(code), &views, players);
            for line in lines {
                self.llog(&line, s);
            }
            why = self.check_times(g, r);
        }
        if !why.is_empty() {
            self.llog(&format!("[online] \"{gname}\": {spers}'s race report not taken: {why}"), s);
            reply(self);
            self.results_table(g, Some(s));
            return self.end_if_back(g);
        }
        let r = r.expect("checked");
        let drift = r.kind == KIND_DRIFT;
        // a drift race: what the reporter's own game says it did, points and its biggest single drift with them (the
        // result line has only the points, and what each report shows of the others is the others' own games' word)
        if drift && let Some(p) = r.players.iter().find(|p| lower(&names[p.index as usize]) == persona) {
            let laps = if r.laps > 0 { format!(", {} lap{}", r.laps, if r.laps == 1 { "" } else { "s" }) } else { String::new() };
            let how = if p.dropped || p.place == 0 { "did not finish".to_string() } else { format!("{} of {}", p.place, r.players.len()) };
            let name = self.lobby.game(g).expect("held").players[me].name.clone();
            self.llog(
                &format!(
                    "[online] \"{gname}\" (drift, track {}{laps}): {name}'s own game: {how}, {} points, its biggest drift {}, top \
                     speed {:.0} km/h, {}",
                    r.track,
                    p.score,
                    p.drift_best,
                    p.top * 3.6,
                    fmt_time(p.time)
                ),
                s,
            );
        }
        // the reporter's own entry in its report (check_report: each index names one of the game's players)
        let own = r.players.iter().find(|p| p.finished() && lower(&names[p.index as usize]) == persona);
        // (its game saw the finishers cross the line: it finished, or the race ended with it on the way)
        let finished = r.players.iter().any(|p| p.raced_to_the_end() && lower(&names[p.index as usize]) == persona);
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
                let place = if p.finished() { p.place as u32 } else { 0 };
                RacePlayer { name, place, time: p.time, left: !p.finished() && p.state != OUT }
            })
            .collect();
        order.sort_by_key(|p| if p.place == 0 { 99 } else { p.place });
        // the report as kept (`finishers_of` is what is compared): the racers who crossed the finish line at their
        // places, then after '|' the others it names
        let others: Vec<&str> = order.iter().filter(|p| p.place == 0).map(|p| p.name.as_str()).collect();
        let key = format!(
            "{}|{}",
            order.iter().filter(|p| p.place != 0).map(|p| format!("{}:{}", p.name, p.place)).collect::<Vec<_>>().join(","),
            others.join(",")
        );
        let decides = finishers_of(&key);
        // (a drift race's finishers by their points, as this report has them)
        let points = |name: &str| r.players.iter().find(|p| lower(&names[p.index as usize]) == lower(name)).map_or(0, |p| p.score);
        let line_of = |order: &[RacePlayer]| {
            order
                .iter()
                .map(|p| {
                    let place = if p.place == 0 { "DNF".to_string() } else { p.place.to_string() };
                    let time = match (p.place, drift) {
                        (0, _) if !p.left => " (out of time)".to_string(),
                        (0, _) => String::new(),
                        (_, true) => format!(" {} points", points(&p.name)),
                        (_, false) => format!(" {}", fmt_time(p.time)),
                    };
                    format!("{place} {}{time}", p.name)
                })
                .collect::<Vec<_>>()
                .join(", ")
        };
        let line = line_of(&order);
        let svip = self.lobby.s(s).vip;
        let gm = self.lobby.game(g).expect("held");
        // the ranked table goes to this player's results screen alone once the result stands, else to every reporter's
        let mut to = None;
        // the players whose reports came before the one that decided the result: their careers went without the race
        let mut before: Vec<(SessId, String)> = Vec::new();
        if !finished || decides.is_empty() {
            if gm.result.is_some() {
                to = Some(s);
            }
            let why = if finished { "'s report has no racer who finished" } else { " did not finish" };
            self.llog(&format!("[online] \"{gname}\": {spers}{why}: the report decides nothing ({line})"), s);
        } else if let Some(result) = &gm.result {
            to = Some(s);
            if finishers_of(result) != decides {
                self.llog(&format!("[online] \"{gname}\": {spers}'s report differs ({line}); the result stands"), s);
            }
        } else if let Some(matched) = gm.reports.iter().find(|(v, k)| finishers_of(k) == decides && *v != svip).map(|(_, k)| k.clone()) {
            // the racers the matching report named and this one left out (a game may leave out one who left early): they
            // did not finish either
            for name in others_of(&matched) {
                if !order.iter().any(|p| lower(&p.name) == lower(name)) {
                    order.push(RacePlayer { name: name.to_string(), place: 0, time: 0.0, left: true });
                }
            }
            let line = line_of(&order);
            let ranked = gm.ranked && order.len() >= 3;
            let was_ranked = gm.ranked;
            let n = order.len();
            let table = gm.ranked.then(|| self.table_frame(g, &r, &names, n, ranked && (r.kind as usize) < RACE_TYPES.len()));
            let far = far_ahead(&gname, r.kind, &order, &self.lobby.game(g).expect("held").players);
            let lobby = &mut self.lobby;
            let players = &lobby.games.get(&g).or_else(|| lobby.dead.get(&g)).expect("held").players;
            let told = far.is_some()
                || order.iter().find(|p| p.place == 1).is_some_and(|w| {
                    players.iter().any(|p| lower(&p.name) == lower(&w.name) && p.own.is_some_and(|o| o.told))
                });
            let quick = quick_win(&mut lobby.wins, &gname, &r, &order, players, told);
            let gm = self.lobby.game_mut(g).expect("held");
            gm.result = Some(key);
            gm.table = table;
            before = gm.players.iter().filter_map(|p| p.results.filter(|&x| x != s).map(|x| (x, p.name.clone()))).collect();
            if let Some(line) = far {
                self.llog(&line, s);
            }
            if let Some(line) = quick {
                self.llog(&line, s);
            }
            self.stats.add_race(&Race { kind: r.kind as u32, ranked, track: r.track as i64, players: order });
            let tail = if r.kind as usize >= RACE_TYPES.len() {
                " (no stats for this race type)"
            } else if was_ranked && !ranked {
                " (a ranked race needs 3 players: no points)"
            } else {
                ""
            };
            let rt = REPORT_TYPES[r.kind as usize];
            // (a drift race's line names its track and laps: its points go by them)
            let at = if drift { format!(", track {}, {} lap{}", r.track, r.laps, if r.laps == 1 { "" } else { "s" }) } else { String::new() };
            self.llog(&format!("[online] \"{gname}\" ({rt}, {}{at}): {line}{tail}", if ranked { "ranked" } else { "unranked" }), s);
        } else {
            if !gm.reports.is_empty() && !gm.reports.iter().any(|(_, k)| finishers_of(k) == decides) {
                self.llog(&format!("[online] \"{gname}\": {spers}'s report differs ({line})"), s);
            }
            self.lobby.game_mut(g).expect("held").reports.push((svip, key));
        }
        reply(self);
        // the game asked for its career (user) at the login, before this report: once more, with the race in it (and to
        // the players who reported before the result stood: the winner, at the results screen first, saw 0 points)
        let pers = self.lobby.s(s).persona.clone().unwrap_or_default();
        self.user_info(s, &pers);
        for (x, name) in before {
            self.user_info(x, &name);
        }
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
                f.extend(fields![("TIME", RESULTS_WAIT_MS)]);
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
    /// place (-2 "Disconnected": did not finish), two numbers the screen does not show (0), the rating points of a race
    /// of n racers (0 when the race gives none)
    fn table_frame(&self, g: GameId, r: &RaceReport, names: &[String], n: usize, points: bool) -> Vec<u8> {
        let hex = |x: i64| if x < 0 { format!("-{:x}", -x) } else { format!("{x:x}") };
        let mut f: Fields = Vec::new();
        for (i, pl) in self.lobby.game(g).expect("held").players.iter().take(6).enumerate() {
            let want = lower(&pl.name);
            let Some(p) = r.players.iter().find(|p| lower(&names[p.index as usize]) == want) else { continue };
            let place = if p.dropped { 0 } else { p.place as u32 };
            let first =
                if REPORT_TYPES[r.kind as usize] == "drift" { p.score as i64 } else { (p.time * 1000.0).round().clamp(0.0, 1e9) as i64 };
            let pts = race_points(points, n, place) as i64;
            let data = format!("{},{},0,0,{}", hex(first), hex(if place == 0 { -2 } else { place as i64 }), hex(pts));
            f.extend(fields![(format!("OPPO{i}"), pl.name.as_str()), (format!("DATA{i}"), data)]);
        }
        encode_frame("$rfn", "", &f).expect("the lobby's own fields")
    }

    /// why a decoded race report is not one of game g's ('' = it is: the game's players, each named once, places
    /// 1..n, a race type).  A report may leave out players of the game: the others' games leave out one who joined as
    /// the race started and never raced, or left it early (nfsu2.xyz, 2026-10-30: 460 reports of 221 races were not
    /// taken for it, and no `[cheat]` line looked at them)
    fn check_report(&self, g: GameId, r: Option<&crate::stats::RaceReport>, names: &[String]) -> String {
        let Some(r) = r else { return "the report does not parse".into() };
        let g = self.lobby.game(g).expect("held");
        if r.players.len() != names.len() || names.len() > g.players.len() {
            return "not the game's number of players".into();
        }
        let mut want: HashSet<String> = g.players.iter().map(|p| lower(&p.name)).collect();
        if !names.iter().all(|n| want.remove(&lower(n))) {
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
        if r.kind as usize >= REPORT_TYPES.len() {
            return format!("race type {}", r.kind);
        }
        String::new()
    }

    /// why a race report of the right shape (check_report) cannot be the result of game g by its times ('' = it can)
    fn check_times(&self, g: GameId, r: &crate::stats::RaceReport) -> String {
        let g = self.lobby.game(g).expect("held");
        let age = (self.now() - g.started) / 1000.0;
        let min = MIN_RACE_S[r.kind as usize];
        if age < min {
            return format!(
                "a race of {} s (a {} race takes {} s at least)",
                js::to_fixed(age, 1),
                REPORT_TYPES[r.kind as usize],
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
    /// two), 24..29 last month's (Last Month's Leaderboard: the month before's counts as they were when it ended;
    /// until 2026-11 the server kept none, and the page was empty; November 2026 is the first month shown, stats.rs
    /// `FIRST_WHOLE_MONTH`).  Each six in the screens' order, Drift before Drag
    /// (`SLOT_TYPES`; until 2026-10-25 read as race types, so the Drift pages showed the drag board); INDEX (the race
    /// type and the player's class, 0x58ce30) only goes back.  Rows: +snp N (name), R (rank), P (points), S
    /// (points,wins,losses), O.  R and P in decimal (SPEED2.EXE 0x7420b0 reads digits 0-9 only; the screen shows P,
    /// sorts its list by it and takes the player's class from it), S in hex (0x7425d0, "l*"; the screen shows its wins
    /// and losses).
    fn snap(&mut self, s: SessId, f: &FieldMap) {
        let chan = jsx::to_int32(f.num("CHAN", f64::NAN));
        let index = f.get("INDEX").map_or(V::N(0.0), V::from);
        let start = 0i32.max(jsx::to_int32(f.num("START", 0.0)));
        let range = 100i32.min(0i32.max(jsx::to_int32(f.num("RANGE", 0.0))));
        // (in 64 bits: CHAN - 6 does not overflow for a CHAN near -2^31)
        let at = chan as i64 - 6;
        let kind = (0..24).contains(&at).then(|| SLOT_TYPES[(at % 6) as usize]);
        let own = (6..12).contains(&at);
        let period = match at {
            12..18 => Period::Month,
            18.. => Period::LastMonth,
            _ => Period::All,
        };
        let find = f.get("FIND").map(str::to_string);
        // the board as last built (stats.rs: at most every few seconds), and its rows by name
        let (rows, total) = {
            let persona = self.lobby.s(s).persona.clone().unwrap_or_default();
            let board = match kind {
                Some(k) => Some(self.stats.ranking(k, period)),
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
        let me = persona.as_deref().map(lower).unwrap_or_default();
        let to = f.get("PRIV").filter(|p| !p.is_empty()).map(lower);
        // a line that reads as a slur: only its sender sees it; slur_lines of them in slur_window_ms mute it
        let slur = nfsu2_names::chat_slur(&text).map(str::to_string);
        if let Some(word) = &slur {
            let l = self.lobby.l;
            let x = self.lobby.sm(s);
            x.slurs.retain(|t| now - t < l.slur_window_ms);
            x.slurs.push(now);
            let muted = x.slurs.len() as f64 >= l.slur_lines;
            if muted {
                x.mute_until = now + l.slur_mute_ms;
                x.slurs.clear();
            }
            let then = if muted { format!("; muted for {} s", js::number_to_string(l.slur_mute_ms / 1000.0)) } else { String::new() };
            let word = serde_json::to_string(word).expect("a string");
            self.llog(&format!("[online] {}: a line with {word} in it, shown to no one else{then}", self.who(s)), s);
        }
        if !text.is_empty() && !me.is_empty() {
            let l = self.lobby.l;
            let said = self.lobby.said.entry(me.clone()).or_default();
            said.push_back(Said { t: now, text: text.clone(), to: to.clone(), hidden: slur.is_some() });
            while said.len() as f64 > l.said_lines {
                said.pop_front();
            }
        }
        let with_attr = |mut v: Fields| {
            if !attr.is_empty() {
                v.extend(fields![("A", attr.clone())]);
            }
            v
        };
        // each hearer's blocked list is looked at only when some list blocks the sender
        let (hidden, blocked) = (slur.is_some(), self.buddies.blocked(&me));
        // to one persona (the game's challenge invites too)
        if let Some(to) = to {
            if let Some(u) = self.lobby.by_persona.get(&to).copied()
                && !self.lobby.s(u).gone
                && self.hears(u, s, &me, hidden, blocked)
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
                if self.hears(p, s, &me, hidden, blocked) {
                    self.lsend_frame(p, &msg);
                }
            }
            return;
        }
        if let Some(r) = room.filter(|r| self.lobby.room(*r).is_some()) {
            let users: Vec<SessId> = self.lobby.room(r).expect("a room").users.iter().copied().collect();
            for u in users {
                if self.lobby.room(r).is_some_and(|x| x.users.contains(&u)) && self.hears(u, s, &me, hidden, blocked) {
                    self.lsend_frame(u, &msg);
                }
            }
        }
    }

    /// session u hears a line of session s (persona `from`, lower case): its sender always; another not a `hidden`
    /// one (it reads as a slur), nor, when some list blocks the sender (`blocked`), one its own persona blocks
    fn hears(&self, u: SessId, s: SessId, from: &str, hidden: bool, blocked: bool) -> bool {
        let blocks = |p: &str| self.buddies.blocks(&lower(p), from);
        u == s || !(hidden || (blocked && self.lobby.s(u).persona.as_deref().is_some_and(blocks)))
    }

    /// "Submit Complaint" about persona PERS: a log line with that player's virtual address (when it is online) and
    /// what it said lately that the complainer could hear: its lines in a room or game, and the private ones to the
    /// complainer (`said`); "Report complete"
    fn complaint(&mut self, s: SessId, f: &FieldMap) {
        let name = clean_text(f.str("PERS"), 16);
        let k = lower(&name);
        let me = self.lobby.s(s).persona.as_deref().map(lower);
        let (now, ms) = (self.now(), self.lobby.l.said_ms);
        let said: Vec<String> = self
            .lobby
            .said
            .get(&k)
            .into_iter()
            .flatten()
            .filter(|x| now - x.t <= ms && (x.to.is_none() || x.to == me))
            .map(|x| serde_json::to_string(&x.text).expect("a string") + if x.hidden { " (shown to no one)" } else { "" })
            .collect();
        let whom = match self.lobby.by_persona.get(&k) {
            Some(&u) if !self.lobby.s(u).gone => self.who(u),
            _ => format!("\"{name}\""),
        };
        let said = if said.is_empty() { " (no lines of its kept)".to_string() } else { format!(", who said: {}", said.join(" ")) };
        self.llog(&format!("[online] {}: a complaint about {whom}{said}", self.who(s)), s);
        self.lsend(s, "rept", &fields![("TEXT", "Report complete")], "")
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
        let only_asked = filter_set && [cf & cm, cm, sf & sm, sm] != FILTER_DEFAULT;
        let start = 0i32.max(jsx::to_int32(f.num("START", 0.0))) as usize;
        let count = (0i32.max(jsx::to_int32(f.num("COUNT", 20.0))) as usize).min(GAMES_PER_UPDATE);
        let x = self.lobby.s(s);
        let (room, mine) = (x.room, x.game);
        let asked = |g: &Game| (g.cust_flags & cm) == (cf & cm) && (g.sys_flags & sm) == (sf & sm);
        let open: Vec<&Game> = self
            .lobby
            .games
            .values()
            .filter(|g| g.started == 0.0 && g.cust_flags & PRIVATE == 0 && (room.is_none() || g.room == 0 || Some(g.room) == room))
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
        // a player under a race ban makes no game (its page told it why: nfsu2-bans)
        if self.races_off(s) {
            self.llog(&format!("[online] {}: under a race ban: made no game", self.who(s)), s);
            return self.lsend(s, "gcre", &[], "full");
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
        // the game names its game after the host ("088.Alpha"), a name the login took; an edited one may send any
        let name = clean_text(f.str("NAME"), self.lobby.l.game_name_len as usize);
        if name.is_empty() || nfsu2_names::blocked_name(&name) {
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
        let sys_flags = jsx::to_uint32(f.num("SYSFLAGS", 0.0)) & !ACTIVE;
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
            sys_flags: (sys_flags & !PASSWORD) | if password.is_some() { PASSWORD } else { 0 },
            password,
            params: jsx::slice16(f.str("PARAMS"), 256).to_string(),
            players: vec![self.member(s)],
            started: 0.0,
            auth: String::new(),
            reports: Vec::new(),
            result: None,
            table: None,
            ranked: sys_flags & RANKED != 0,
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
            own: None,
            seen: Vec::new(),
        }
    }

    /// a game's record to everyone looking at the game list in its room who is not in it (their lists update), at
    /// most every announce_ms: the latest
    fn announce(&mut self, g: GameId, gone: bool) {
        let Some(x) = self.lobby.game(g) else { return };
        // not on the game list
        if x.cust_flags & PRIVATE != 0 {
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
        // nor joins one
        if self.races_off(s) {
            self.llog(&format!("[online] {}: under a race ban: joined no game", self.who(s)), s);
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
            let flags = jsx::to_uint32(f.num("USERFLAGS", f64::NAN)) & READY;
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
        let l = self.lobby.l;
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
                let v = (x.sys_flags & !COUNTDOWN) | (jsx::to_uint32(f.num("SYSFLAGS", f64::NAN)) & COUNTDOWN);
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
        x.sys_flags |= ACTIVE;
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
        self.lobby.said.retain(|_, v| {
            while v.front().is_some_and(|x| now - x.t > l.said_ms) {
                v.pop_front();
            }
            !v.is_empty()
        });
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

    // a kept report's finishers and the others it names, as kept now and as races.json kept them before 2026-11
    #[test]
    fn kept_reports() {
        assert_eq!(finishers_of("Bob:1,Ann:2|Cat,Dan"), "Bob:1,Ann:2");
        assert_eq!(others_of("Bob:1,Ann:2|Cat,Dan"), ["Cat", "Dan"]);
        assert_eq!(finishers_of("Bob:1,Ann:2,Cat:0"), "Bob:1,Ann:2");
        assert_eq!(others_of("Bob:1,Ann:2,Cat:0"), ["Cat"]);
        assert_eq!((finishers_of("Bob:1|"), others_of("Bob:1|")), ("Bob:1".to_string(), vec![]));
        assert_eq!(finishers_of("|Ann,Bob"), "");
    }
}
