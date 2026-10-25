//! A game's Online mode as data (its title): what the online host says and checks that is the game's own.  The host
//! itself (the framing, the virtual host, the services and their rules) is the same for every title; one server
//! serves one title, the one `OnlineHost::spawn_for` was given (`OnlineHost::spawn`: NFS Underground 2's, [`UG2`]).
//! Each value comes from the game's own lobby client (its parsers in the exe) and what the game was seen to do,
//! never from a guess: a game whose values are not all known has no title yet.

use crate::http::{NEWS, Page};
use crate::stats::{RaceReport, Records, Table, decode_result, stat_v38};

/// a value of the server config (the "news" reply, code new7)
#[derive(Clone, Copy, Debug)]
pub enum Cfg {
    /// this text
    Text(&'static str),
    /// the host's EA Messenger port (PORT_BUDDY)
    BuddyPort,
    /// how many games one game list search lists (lobby.rs GAMES_PER_UPDATE)
    GamesPerUpdate,
}

/// the bits of a game's flags that the lobby reads or sets
#[derive(Clone, Copy, Debug)]
pub struct Flags {
    /// OPFLAG bit of a player who is ready (gset USERFLAGS)
    pub ready: u32,
    /// SYSFLAGS bit of a game with a password: the server's own (the game's gcre never sets it)
    pub password: u32,
    /// SYSFLAGS bit of a started game
    pub active: u32,
    /// SYSFLAGS bit of a ranked game
    pub ranked: u32,
    /// SYSFLAGS bit of a game counting down to its start
    pub countdown: u32,
    /// CUSTFLAGS bit of a private game (never on the game list)
    pub private: u32,
}

/// One game's Online mode.
pub struct Title {
    /// the host's name on the switch's peer lists
    pub(crate) vhost_name: &'static str,
    /// the port the game asks for the lobby's address on (the redirector: @dir)
    pub(crate) port_redirector: u16,
    /// the rooms everyone shares, and what a room's record says of its game (H) and its flags (F)
    pub(crate) rooms: &'static [&'static str],
    pub(crate) room_h: &'static str,
    pub(crate) room_f: &'static str,
    /// SERV in a persona's record (user)
    pub(crate) serv: &'static str,
    /// the server config the game asks for at connect, in its order (lobby.rs `config_fields`)
    pub(crate) config: &'static [(&'static str, Cfg)],
    pub(crate) flags: Flags,
    /// the Filter's defaults as a search asks for them: CUSTFLAGS, CUSTMASK, SYSFLAGS, SYSMASK
    pub(crate) filter_default: [u32; 4],
    /// the race types that have stat blocks, by the game's own numbers (stats.json keeps the counts by these numbers,
    /// six at most)
    pub(crate) race_types: &'static [&'static str],
    /// the race types a report may name (race_types first), and the shortest race of each, in seconds, that a result
    /// may come from
    pub(crate) report_types: &'static [&'static str],
    pub(crate) min_race_s: &'static [f64],
    /// the game's race report (rank RESU) decoded: None for anything that does not parse
    pub(crate) decode_report: fn(&str) -> Option<RaceReport>,
    /// the game's STAT field from a persona's counts per race type and its records
    pub(crate) stat: fn(&Table, &Records) -> String,
    /// the Rankings screens' six boards in their order, as race types
    pub(crate) slot_types: [u32; 6],
    /// what a ranked race's results screen counts down from until its table comes, in ms
    pub(crate) results_wait_ms: f64,
    /// EA Messenger: TITL and EXTR in a persona's presence
    pub(crate) messenger_titl: &'static str,
    pub(crate) messenger_extr: &'static str,
    /// the news page the config's address names (http.rs)
    pub(crate) news: &'static Page,
}

/// the tier points of each race type (UG2's server config)
const UG2_TIER: &str = "0,1999,4999,9999,19999,39999,59999,79999,99999,119999";

/// NFS Underground 2 (SPEED2.EXE)
pub const UG2: Title = Title {
    vhost_name: "NFSU2 Online",
    // (pcnfs05.ea.com's port, SPEED2.EXE 0x7FBE00)
    port_redirector: 20920,
    // a lounge and one per race type
    rooms: &["Lounge", "Circuit", "Sprint", "Drift", "Drag", "StreetX", "URL"],
    room_h: "NFSU2",
    room_f: "CK",
    serv: "NFSU2",
    // where the buddy server is (here: this same host), the news and terms pages, the game list's page, the tier
    // points per race type
    config: &[
        ("TOSURL", Cfg::Text("http://10.77.0.1/tos")),
        ("NEWSURL", Cfg::Text("http://10.77.0.1/news")),
        ("BUDDY_SERVER", Cfg::Text("10.77.0.1")),
        ("BUDDY_PORT", Cfg::BuddyPort),
        ("GAMES_PER_UPDATE", Cfg::GamesPerUpdate),
        ("CIRCUIT_TIER_POINTS", Cfg::Text(UG2_TIER)),
        ("SPRINT_TIER_POINTS", Cfg::Text(UG2_TIER)),
        ("DRIFT_TIER_POINTS", Cfg::Text(UG2_TIER)),
        ("DRAG_TIER_POINTS", Cfg::Text(UG2_TIER)),
        ("STREET_CROSS_TIER_POINTS", Cfg::Text(UG2_TIER)),
        ("URL_TIER_POINTS", Cfg::Text(UG2_TIER)),
    ],
    flags: Flags {
        // (server.dll)
        ready: 1 << 27,
        // the game's game list asks for the password of a game that has it before it joins (0x4f1713); a wrong one
        // is "pass" (0x49ebc3, 0x55911e)
        password: 1 << 16,
        active: 1 << 19,
        // (Options: Game Type Ranked; the game then asks for 3 players at least, and its lobby's default search asks
        // for unranked games: SYSMASK bits 18 and 19; gcre sets it, 0x58d580)
        ranked: 1 << 18,
        // (the creator's gset; server.dll takes the creator's SYSFLAGS): Start Race while some players are ready and
        // others not sets it (SPEED2.EXE 0x58aa20), every game in the room then counts down 10 s (0x4d2852, 0x4e5982),
        // and the creator's game kicks the players still not ready (gset KICK, a list) and starts (0x58a7e0); a start
        // that fails clears it (0x58ab40)
        countdown: 1 << 12,
        // (Options: Private Room, SPEED2.EXE 0x58d55d; the game's own search always asks for it clear, 0x590669: its
        // players come by invitation)
        private: 1 << 8,
    },
    // Circuit, no car restriction, Collision Detection and Race Breaker on, unranked, not private
    filter_default: [0x40000f3, 0x40001f3, 0, 0xc0000],
    // the type in its race report (SPEED2.EXE 0x5fe8d0, from the game's CUSTFLAGS race bit, 0x58b0f7) and the order
    // it reads the server config's tier points in (0x58a370); its stat blocks are taken to follow it (UNVERIFIED).
    // (Until 2026-10-19 drag and drift were the other way round: stats.json is kept by these numbers, so only the
    // names changed.)
    race_types: &["circuit", "sprint", "drag", "drift", "streetx", "url"],
    // and the game's seventh (a game with CUSTFLAGS bit 25, which SPEED2.EXE 0x5fe8d0 reports as 6), which has no stat
    // block or tier points: its results count nowhere
    report_types: &["circuit", "sprint", "drag", "drift", "streetx", "url", "type 6"],
    // well under the game's own shortest (one lap of the shortest track; a quarter-mile drag)
    min_race_s: &[20.0, 20.0, 5.0, 20.0, 20.0, 20.0, 20.0],
    // version 3 (stats.rs)
    decode_report: decode_result,
    // 38 numbers (stats.rs)
    stat: stat_v38,
    // Circuit, Sprint, Drift, Drag, Street X, URL (SPEED2.EXE 0x58ce30, 0x582530) as race types: drag 2, drift 3
    slot_types: [0, 1, 3, 2, 4, 5],
    // ("Waiting 60 seconds for results", then "Waiting for final server results"; SPEED2.EXE 0x4e9210): a guess at how
    // long the others take to finish and report, no limit (the table comes once the result stands)
    results_wait_ms: 60_000.0,
    messenger_titl: "Need for Speed Underground 2 [PC]",
    messenger_extr: "NFS-CONSOLE-2005",
    news: &NEWS,
};

// stats.json keeps six race types, and each report type has its shortest race
const _: () = assert!(UG2.race_types.len() <= 6 && UG2.min_race_s.len() == UG2.report_types.len());
