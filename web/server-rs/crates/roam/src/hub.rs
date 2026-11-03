//! The hub's logic, without its timer (`RoamHub` gives it one): the players, the links, the cap.
//!
//! The order of the players, and of each player's links, is the order they came in: it decides between pairs at one
//! distance, and so the links.  The players live in slots (a link names the slot of the player it is to: a player
//! that goes is taken out of every link before its slot is used again, at the next tick or once many have gone, and
//! until then links to it count for nothing) and `order` lists the slots in the order the players came.

use std::collections::{HashMap, HashSet};
use std::time::Instant;

use bytes::{Bytes, BytesMut};
use nfsu2_util::json::num;
use nfsu2_util::{js, vip_str};
use serde_json::{Value, json};

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use crate::links::{
    Cand, Found, Grid, Heap, Level, Near, Place, WALK_AHEAD, WALK_SOON, WALK_VIEW, before, distance, order_key, sort_stable, square, weigh,
};
use crate::results::{self, Results};
use crate::watch::{self, Seen, Watch};
use crate::wire::{self, F_DATA, KIND_UDP, Msg};
use crate::{PeerEntry, ROAM_HUB_VIP, RoamOptions};

/// no slot, no place in a room
const NONE: u32 = u32::MAX;

/// a race lobby a player hosts (roam_core.h RC_MSG_LOBBY, an advert to the hub): its id, the race, where it starts,
/// its players (n, the host first), the most it takes and its state
#[derive(Clone, Copy, Debug)]
struct HubLobby {
    id: u32,
    race: u32,
    x: i16,
    y: i16,
    n: u8,
    max: u8,
    state: u8,
    ids: [u32; wire::LOBBY_PLAYERS],
}

/// MAPD: the page moves each player's marker on from where the hub last said, at its velocity; the hub says it again
/// once the marker would be this far (m) from where the player is, or its heading this far off (256 a turn, 45°).  One
/// pixel or two on the whole city's map, a car turning off now and then, a car standing never: four answers a second
/// take about half the bytes the whole list once a second did for cars on a city's roads (30 % of them standing, the
/// rest turning a corner every 3 to 11 s, SNAPs twice a second), and as many for cars going round circles of 40 m all
/// the time (nfsu2-roamload's).
const MAP_TOL_M: f64 = 8.0;
const MAP_TOL_H: u8 = 32;
/// MAPD: a player's last SNAP moved on at its velocity this long at most (two SNAPs' time)
const MAP_AHEAD_MS: f64 = 1000.0;
/// a browser's world map not asked for this long is forgotten (its next answer starts from nothing); its race lobbies
/// go with every answer a second at most
const MAP_KEEP_MS: f64 = 10_000.0;
const MAP_LOBBIES_GAP_MS: f64 = 1000.0;
/// MAPD, to a map that asks (wire::MAPREQ_PARKED): a player whose SNAPs have stayed within STAY_M of one spot for
/// PARKED_MS is parked (wire::MAPD_PARKED in its entry's key): the world map counts those for a car meet, and a busy
/// spot of players passing or just come (a new career's start) is none
const STAY_M: f64 = 25.0;
const PARKED_MS: f64 = 60_000.0;
/// MAPD's most players placed in one answer (MAPD_PARTS full datagrams)
const MAPD_MAX: usize = wire::MAPD_PARTS * (wire::MAPD_BYTES / wire::MAPD_PLACE);
/// ...with their heights (an answer to wire::MAPREQ_PARKED)
const MAPD_MAX_Z: usize = wire::MAPD_PARTS * (wire::MAPD_BYTES / wire::MAPD_PLACE_Z);
/// NEARD (the minimap's answer, roam_game.c near_*): one datagram, so 128 players placed at most, within NEAR_M at
/// most of the asker (roam_core.h RC_NEAR_MAX, RC_NEAR_M).  The minimap shows a few hundred metres across a dozen
/// pixels' arrows: a marker is placed again once it would be 2 m off (a pixel or two there) or its heading 11° off
const NEAR_MAX: usize = wire::MAPD_BYTES / wire::MAPD_PLACE;
const NEAR_M: f64 = 1000.0;
const NEAR_TOL_M: f64 = 2.0;
const NEAR_TOL_H: u8 = 8;
/// NEARD: the cells (m) of each room's grid of the players the maps show, made at the first question after a tick
const NEAR_CELL_M: f64 = 128.0;
/// ...where a player may have gone since (the grid is up to a tick old): its cells are looked at this much farther out
const NEAR_SLACK_M: f64 = 64.0;
/// NEARD: a browser's candidates are picked again every NEAR_CANDS_MS, or once its player is NEAR_MOVE_M from where it
/// was then, of the players this much farther than its radius too (two cars at 60 m/s close 120 m in the second)
const NEAR_KEEP_M: f64 = 120.0;
const NEAR_CANDS_MS: f64 = 1000.0;
const NEAR_MOVE_M: f64 = 40.0;
/// NEARD: a browser's candidates come from the cells this many cells round its own first (192 m all round it, the
/// slack taken off); only when fewer than it needs are that near, from the rest of its reach too
const NEAR_FIRST: i32 = 2;
/// NEARD: the hub's own thread this busy (ticks and frames, of the last second): one answer a browser every 500 ms
/// at most, and this busy every 1000 ms (bench.rs bench_near: 12,000 players in 4 km asking 4 times a second took
/// 0.46 s of each second, the ticks 0.43 more)
const NEAR_BUSY_LOW: f64 = 0.6;
const NEAR_BUSY_HIGH: f64 = 0.75;

/// a player of a room the world map shows (MAPD): its slot (the key) and id, where it is, as sent
#[derive(Clone, Copy)]
struct MapCand {
    slot: u32,
    id: u32,
    p: [f64; 3],
    x: i16,
    y: i16,
    heading: u8,
    vx: i8,
    vy: i8,
    /// it has stood at one spot for PARKED_MS
    parked: bool,
}

/// a player as a browser's world map was last told (MAPD): its place, heading and velocity then, and when; parked as
/// its key said
#[derive(Clone, Copy)]
struct MapSent {
    slot: u32,
    id: u32,
    x: i16,
    y: i16,
    heading: u8,
    vx: i8,
    vy: i8,
    parked: bool,
    t: f64,
}

/// NEARD: every room's players the maps show at one moment (`at`), sorted by room, cell row and cell (`keys`, so that
/// a row's cells are one run), each with where it was then
struct NearGrid {
    at: f64,
    /// which grid of the hub's this is (one more each time one is made)
    nth: u64,
    keys: Vec<(u32, i32, i32)>,
    /// each (room, cell row) of `keys` and where its run starts there, in their order: a row's cells are looked for in
    /// its own run (two searches of all the keys for each row read were most of picking a crowd's candidates)
    rows: Vec<(u32, i32, u32)>,
    /// by place in `items`, what an answer reads of that player now (`Shown`: made with the grid, then kept as it
    /// changes, `Hub::shown_sync`): an answer's players are near each other in the grid, so their reads are too, where
    /// by slot they were anywhere (most of an answer's time at 12000 players)
    live: Vec<Shown>,
    items: Vec<NearItem>,
    /// each slot's place in `items` (u32::MAX: none)
    pos: Vec<u32>,
}

/// What a minimap answer reads of each player it places (`neard`: `map_shown`, `ahead`, `map_cand`): a line of memory
/// each, in the minimap grid's order (`NearGrid::live`), kept as it changes (`Hub::shown_sync`, after each frame of the
/// player's).  The players' own fields are spread over 8 MB of slots at 12000 players, and an answer reads 64 of them.
#[derive(Clone, Copy, Default, PartialEq, Debug)]
#[repr(align(64))]
struct Shown {
    /// a player in the slot
    live: bool,
    /// `Player::p` is some (`p` is its value, else zeros)
    has_p: bool,
    impostor: bool,
    status: u8,
    heading: u8,
    id: u32,
    room: u32,
    p: [f64; 3],
    v: [f64; 2],
    pos_at: f64,
}

impl Shown {
    fn of(o: &Player) -> Shown {
        Shown {
            live: true,
            has_p: o.p.is_some(),
            impostor: o.impostor,
            status: o.status,
            heading: o.heading,
            id: o.id,
            room: o.room,
            p: o.p.unwrap_or_default(),
            v: [o.v[0], o.v[1]],
            pos_at: o.pos_at,
        }
    }

    /// the same, bit for bit (a NaN the same NaN)
    fn same(a: &Shown, b: &Shown) -> bool {
        let bits = |s: &Shown| {
            (
                (s.live, s.has_p, s.impostor, s.status, s.heading, s.id, s.room),
                s.p.map(f64::to_bits),
                s.v.map(f64::to_bits),
                s.pos_at.to_bits(),
            )
        };
        bits(a) == bits(b)
    }
}

/// a player of the NEARD grid: its slot, where it was at the grid's making (moved on from its SNAP, `ahead`), its
/// velocity, and how long more it may be moved on (s: what is left of MAP_AHEAD_MS)
#[derive(Clone, Copy)]
struct NearItem {
    slot: u32,
    x: f32,
    y: f32,
    vx: f32,
    vy: f32,
    left: f32,
}

/// a browser's world map (MAPD): the answer it last said it holds and the one sent since (each its req and its players
/// by slot), and when it last asked
#[derive(Default)]
struct MapHeld {
    held: Option<(u16, Vec<MapSent>)>,
    sent: Option<(u16, Vec<MapSent>)>,
    at: f64,
    /// NEARD: the players the minimap's answers pick from (by slot), and when, for which slot, radius and most they were
    /// picked and where the asker was (NEAR_CANDS_MS, NEAR_MOVE_M)
    cands: Vec<u32>,
    cands_of: Option<(f64, u32, u16, u16, f64, f64)>,
    /// the same as places in the grid `cand_nth` (`NearGrid::nth`), in their order: a question reads its candidates
    /// from a few runs of the grid, not from anywhere in it (the grid has a cell's players one after another)
    cand_k: Vec<u32>,
    cand_nth: u64,
}

/// an advert older than this is a lobby gone (its host sends one every second)
const LOBBY_STALE_MS: f64 = 3000.0;
/// A lobby's host is linked with the players in free roam this near its start before any other link (the nearest
/// LOBBY_NEAR_MAX of them, three quarters of the 32): any of them may join it (roam_game.c MP_JOIN_M, 15 m from the
/// start, as the host sees them)
const LOBBY_NEAR_M: f64 = 20.0;
const LOBBY_NEAR_MAX: usize = 24;
/// a player's ask to join a lobby (its JOIN's copy to the hub) links it with the host first for this long (it asks
/// again every half second while it waits; once in, it is one of the lobby's players)
const LOBBY_JOIN_MS: f64 = 6000.0;
/// a lobby's host, and each player it lists, are this near its start, or the advert is no reason for a link: the host
/// game drops a player of its lobby who is farther (roam_game.c MP_STAY_M + 10 m), and leaves the lobby itself past
/// MP_STAY_M (40 m)
const LOBBY_STAY_M: f64 = 50.0;
/// a player's race reports logged: one a race (its lobby), and none sooner than this after its last
const REPORT_GAP_MS: f64 = 20000.0;
/// how far a race lobby's start may be from where a cheat's car came to for its line to name it (m): a player 15 m
/// from the start may join (roam_game.c MP_JOIN_M)
const LOBBY_LAND_M: f64 = 30.0;
/// the other players next to where a cheat's car came to: by their SNAPs of the last this many ms, at most NEAR_NAMED
/// of them named
const NEAR_FRESH_MS: f64 = 2000.0;
const NEAR_NAMED: usize = 3;
/// A player gone without its BYE (silent for gone_ms, or off the switch) keeps its cheat watch this long (ms): back on
/// the hub in that time (a bad connection's page that stalled, or dropped and came again), it goes on with it, its
/// counts and where its car was; its summary comes once that time is past.  A bad connection had its players gone
/// and back every half minute, and a watch of their own each time saw no fast clock (it takes a minute), told every
/// teleport as the first and summed up every time; and going off the switch would have been a way past the watch.
const WATCH_KEEP_MS: f64 = 60_000.0;

/// a watch kept for a player gone without its BYE: its game's id, its name, since when
struct KeptWatch {
    watch: Watch,
    id: u32,
    name: String,
    at: f64,
}
/// A race lobby the hub saw, for its races' results (results.rs): the race, the browser that hosts it, its host and
/// the players whose own ask to join it came to the hub (a host's advert names whom it likes), and when it was last
/// heard of.  A RACERES is taken only from one of them, naming only them: no player can be named in a race it was
/// not in.  Kept JOINED_KEEP_MS after its last advert or ask (a race and the way back take minutes), at most
/// JOINED_PER_VIP a browser (its newest) and JOINED_IDS players a lobby (those who left before it started count too).
struct Joined {
    race: u32,
    vip: u32,
    ids: Vec<u32>,
    at: f64,
}
const JOINED_KEEP_MS: f64 = 1_800_000.0;
const JOINED_PER_VIP: usize = 4;
const JOINED_IDS: usize = 12;
/// Two players of a room who share a crew are linked after the race lobbies' links and before all others, each
/// player with at most this many such links (`crew_pairs`): half of the 32, so that the cars around it still show
/// (under the busy cap, 8 links a player, a crew of 9 or more in one place takes all of them); half of its own links
/// for a player that asks for fewer than 32 (a phone's 16: 8)
const CREW_LINKS: u32 = 16;
fn crew_cap(degree: usize) -> u32 {
    CREW_LINKS.min((degree / 2).max(1) as u32)
}
/// a crew of more players than this in one room gets no links first (its pairs grow as the square of its players:
/// 64 are 2016), its players link by distance as everyone does, and it counts in `crewSkips`
const CREW_GROUP_MAX: usize = 64;
/// the friends a browser's list here holds at most (the server's lists hold 50, and others' friendships add to it)
const FRIENDS_MAX: usize = 64;
/// how near two players' cars are, at most, for one to ask the other to be friends (`linked_with`): their cars touched
/// front bumpers, and a SNAP is a few hundred ms old at most (30 m: 100 km/h for a second)
const FRIEND_NEAR_M: f64 = 30.0;

/// A player's race report as a log line (one each player of a race, back in free roam: the lines of one race share
/// its lobby), for evening out the cars: the race, how the player did, the car as its game rated it.  Grep `[race]`.
fn race_line(player: &str, r: &wire::RaceRep) -> String {
    let mode = results::mode_name(r.mode);
    let how = place_word(r);
    let time = if r.time > 0.0 { format!(" in {:.2} s", r.time) } else { String::new() };
    let name: String = r.name.iter().map(|&c| c as char).collect();
    format!(
        "[race] lobby {:08x}, race {:08x} ({mode}, track {}, {} lap{}, {} cars): {player}{} {how}{time}; {name} (type {}), \
         class {}, rating {:.1} (the cars' average {:.1}), {:.0} kW, {:.0} kg, {:.0} km/h, power x{:.3}{}",
        r.lobby,
        r.race,
        r.track,
        r.laps,
        if r.laps == 1 { "" } else { "s" },
        r.players,
        if r.host { " (host)" } else { "" },
        r.car_type,
        r.class,
        r.rating,
        r.field,
        r.kw,
        r.kg,
        r.kmh,
        r.factor,
        match r.credit {
            1 => "; won it in the career",
            2 => "; a friend's win in the career",
            _ => "",
        }
    )
}

/// what no car of the game has, however tuned: engine power (kW), top speed (km/h), the game's rating (nfsu2.xyz,
/// 2026-10-30: of 8,143 cars in lobby races the most were 452 kW, 381 km/h and a rating of 804; an edited save's 350Z
/// had 2351 kW, 444 km/h and 639 kg, rated 5000, the most a report carries, and won lobby races with it)
const CAR_KW: f64 = 600.0;
const CAR_KMH: f64 = 420.0;
const CAR_RATING: f64 = 1000.0;

/// whether a race report's car is one no tuning makes (CAR_KW, CAR_KMH, CAR_RATING)
fn over_the_game(r: &wire::RaceRep) -> bool {
    r.kw >= CAR_KW || r.kmh >= CAR_KMH || r.rating >= CAR_RATING
}

/// the honest times of the lobbies' races an edited car's is compared with (`Times`): the last TIMES_KEPT of each race
/// (its id and laps), of at most TIMES_RACES races since the start
const TIMES_KEPT: usize = 1024;
const TIMES_RACES: usize = 2048;
/// an edited car's time is compared with the honest cars' rated within LIKE_RATING of it, when there are LIKE_MIN of
/// them, else with every honest car's of the race, when there are LIKE_MIN
const LIKE_RATING: f64 = 60.0;
const LIKE_MIN: usize = 10;
/// A car whose game data is edited but whose numbers any tuning makes is a `[cheat]` line only when its time beat this
/// share of the honest cars' rated like it (LIKE_RATING) in the same race: the edit won it time no driving of that car
/// does.  An edit may gain nothing (nfsu2.xyz, 2026-10-31: five such cars, an RX8 of 236 kW, a Miata of 96 kW, a
/// 240SX, a Civic and a Mustang; four beat 11 to 55 % of the honest cars rated like them, the fifth 83 % of any car's,
/// too few being rated like it).  Every car beats most of any rating's when it is a strong one: that never decides.
const GAIN_SHARE: f64 = 0.95;
const MODE_DRIFT: u8 = 3;

/// The honest finishers' times of the lobbies' races, per race (id, laps), the newest last: (rating, s) of each car
/// whose game data is the game's and whose numbers any tuning makes, in a race of two cars or more, as its own game
/// timed it (a drift race is won by points: none of it)
#[derive(Default)]
struct Times(HashMap<(u32, u8), std::collections::VecDeque<(f32, f32)>>);

/// what a finisher's time beat (`Times::compare`): how many of how many honest times, their median (s), and the
/// ratings they were of (None: any car's, too few rated like it)
struct Compared {
    beat: usize,
    of: usize,
    median: f64,
    like: Option<(f64, f64)>,
}

impl Times {
    fn add(&mut self, r: &wire::RaceRep) {
        if r.edited || over_the_game(r) || r.players < 2 || r.place == 0 || r.time <= 0.0 || r.mode == MODE_DRIFT {
            return;
        }
        let key = (r.race, r.laps);
        if self.0.len() >= TIMES_RACES && !self.0.contains_key(&key) {
            return;
        }
        let v = self.0.entry(key).or_default();
        if v.len() >= TIMES_KEPT {
            v.pop_front();
        }
        v.push_back((r.rating as f32, r.time as f32));
    }

    /// what a finisher's time beat: the honest cars' of its race rated like it, else any car's, LIKE_MIN of them at
    /// least; None when too few, or the report has no time to compare (not finished, a drift race)
    fn compare(&self, r: &wire::RaceRep) -> Option<Compared> {
        if r.place == 0 || r.time <= 0.0 || r.mode == MODE_DRIFT {
            return None;
        }
        let all = self.0.get(&(r.race, r.laps))?;
        let (lo, hi) = (r.rating - LIKE_RATING, r.rating + LIKE_RATING);
        let near: Vec<f64> = all.iter().filter(|x| (lo..=hi).contains(&f64::from(x.0))).map(|x| f64::from(x.1)).collect();
        let (mut t, like) =
            if near.len() >= LIKE_MIN { (near, Some((lo.max(0.0), hi))) } else { (all.iter().map(|x| f64::from(x.1)).collect(), None) };
        if t.len() < LIKE_MIN {
            return None;
        }
        t.sort_by(f64::total_cmp);
        let beat = t.iter().filter(|&&x| x > r.time).count();
        Some(Compared { beat, of: t.len(), median: t[t.len() / 2], like })
    }
}

/// an edited car's time against the honest ones (`Times::compare`), for its lines: "its 50.90 s beat 48 % of the 282
/// honest cars rated 505-625 in this race (their median 51.91 s)"
fn compared_text(r: &wire::RaceRep, c: Option<&Compared>) -> String {
    if r.mode == MODE_DRIFT {
        return "a drift race, won by points: no time to compare".into();
    }
    if r.place == 0 || r.time <= 0.0 {
        return "no time to compare".into();
    }
    let Some(c) = c else { return format!("its {:.2} s: too few honest times of this race to compare it with", r.time) };
    let share = if c.beat == c.of { format!("all {}", c.of) } else { format!("{} % of the {}", c.beat * 100 / c.of, c.of) };
    let cars = match c.like {
        Some((lo, hi)) => format!("honest cars rated {lo:.0}-{hi:.0}"),
        None => "honest cars of any rating (too few rated like it)".to_string(),
    };
    format!("its {:.2} s beat {share} {cars} in this race (their median {:.2} s)", r.time, c.median)
}

/// A player's race report's `[cheat]` line (`who` its address and name, `10.77.3.41 "Bob"`) when its car is one no
/// tuning makes (an edited save's), or its game says the car's data is edited (an edited GlobalB's: a guide passed
/// among players, 2026-10-31, has an AI agent make one car type 3,600 kW with x2.5 grip and brakes in the browser's copy
/// of the file; the game sums each type's record, mprace.c) and its time beat GAIN_SHARE of the honest cars' rated like
/// it (`c`); each line says what its time beat.  Else, for a car of edited data, what its `[race]` line adds ("" for
/// any other).
fn edited_car_line(who: &str, r: &wire::RaceRep, c: Option<&Compared>) -> (Option<String>, String) {
    let over = over_the_game(r);
    if r.players < 2 || !(over || r.edited) {
        return (None, String::new());
    }
    let gain = c.is_some_and(|c| c.like.is_some() && c.beat as f64 >= GAIN_SHARE * c.of as f64);
    let cmp = compared_text(r, c);
    if !over && !gain {
        return (None, format!("; its game has the car's data edited (GlobalB), no gain seen: {cmp}"));
    }
    let mut why = Vec::new();
    if r.edited {
        why.push("its game has the car's data edited (GlobalB)".to_string());
    }
    if over {
        why.push(format!("no car of the game makes {CAR_KW:.0} kW, {CAR_KMH:.0} km/h or a rating of {CAR_RATING:.0}"));
    }
    let name: String = r.name.iter().map(|&c| c as char).collect();
    let line = format!(
        "[cheat] {who}: raced in the race of lobby {:08x} (race {:08x}: {}, {} lap{}) in an edited car, {}: {name} (type {}), \
         {:.0} kW, {:.0} kg, {:.0} km/h, rated {:.0}; {}; {cmp}",
        r.lobby,
        r.race,
        results::mode_name(r.mode),
        r.laps,
        if r.laps == 1 { "" } else { "s" },
        place_word(r),
        r.car_type,
        r.kw,
        r.kg,
        r.kmh,
        r.rating,
        why.join("; ")
    );
    (Some(line), String::new())
}

/// how a race report's player did: "2nd of 4", "did not finish", "quit"
fn place_word(r: &wire::RaceRep) -> String {
    match r.place {
        0 if r.quit => "quit".to_string(),
        0 => "did not finish".to_string(),
        n => format!(
            "{n}{} of {}",
            match n {
                1 => "st",
                2 => "nd",
                3 => "rd",
                _ => "th",
            },
            r.players
        ),
    }
}

/// how often a player's claim hears that it plays (the claim holds two years from its last game: an hour is plenty)
const SEEN_EVERY_MS: f64 = 600_000.0;

/// the console's lines a page gets of the other names its game shows (`Hub::name_check`), at most: a game may name
/// itself anew with each HELLO
const NAME_LINES: usize = 4;
/// the longest name a game shows (roam_core.h RC_NAME_MAX)
const GAME_NAME_MAX: usize = 24;

/// A browser's page as the switch named it (`Hub::page_name`): the name it goes by there, and the other names its
/// game showed that the console was told of
struct Page {
    name: String,
    told: Vec<String>,
    /// its game said its car flew (`flew`): told once
    flew: bool,
}

/// other players within this of a car that flew (m, along the ground) saw it (`Hub::flew`)
const FLEW_SEEN_M: f64 = 200.0;

/// whether a game shows the name `page` (a page's on the switch) as `game`: as roam_core.c `rc_name_clean` makes it,
/// printable ASCII ('?' for any other character) of at most GAME_NAME_MAX, in any case
fn same_name(page: &str, game: &str) -> bool {
    let shown: String = page.chars().map(|c| if (' '..='~').contains(&c) { c } else { '?' }).take(GAME_NAME_MAX).collect();
    shown.trim().eq_ignore_ascii_case(game.trim())
}

/// a link: the player it is to, and since when
#[derive(Clone, Copy, Debug)]
struct Link {
    slot: u32,
    since: f64,
}

/// (In this order, the fields a map's answer reads of every player it places first: `map_shown`, `ahead`,
/// `map_cand`.  They are a line or two of memory; Rust's own order had them over three or four of its 656 bytes, and a
/// minimap answer reads 64 players' of them, one after another anywhere in the slots.)
#[repr(C)]
struct Player {
    p: Option<[f64; 3]>,
    /// its velocity and heading (256 a turn) as of the position
    v: [f64; 3],
    pos_at: f64,
    /// where its SNAPs have stayed within STAY_M of since `stay_at` (x, y): parked after PARKED_MS
    stay: [f64; 2],
    stay_at: f64,
    id: u32,
    room: u32,
    /// its name is claimed by another keyphrase (nfsu2-claims): it gets no links, shows on no map and counts nowhere,
    /// as of the claims' `generation` in `checked` (0: not looked at yet)
    impostor: bool,
    /// as the player last sent it (roam_core.h RC_ST_*: 2 in free roam, 5 paused there): the world map and
    /// `in_roam()` read it
    status: u8,
    heading: u8,
    /// where its car's nose points up or down (wire `slope`, %): where its places ahead are (`prepare`)
    slope: i8,
    vip: u32,
    port: u16,
    /// the hub's port it last wrote to (the source port of its list)
    hub_port: u16,
    /// kept as the player last sent it (its race reports' lines name it)
    name: String,
    /// its name's Online form in lower case (nfsu2-claims `canonical`; empty: nothing of a name): blocks go by it
    canon: String,
    degree: usize,
    seen: f64,
    /// its HELLO says it takes race lobbies (its map questions get the room's lobbies too)
    lobbies: bool,
    /// ...and reads the hub's crew marks (wire::HF_HUBCREW): its friends are marked in its lists even out of crews
    marks: bool,
    /// its crews as its last HELLO named them (wire::HF_CREW), each once, 0 after them: its crew-mates of its room are
    /// linked with it first (`crew_pairs`), and /players counts them (`crew_counts`)
    crews: [u32; wire::CREWS],
    /// the race lobby it hosts, as its last advert to the hub had it, and when that came
    lobby: Option<(HubLobby, f64)>,
    /// the lobby it asked to join (its id), and when
    join: Option<(u32, f64)>,
    /// the other player of its outrun, as its last HELLO named it (wire `Msg::Hello::duel`; 0: none): the two are
    /// linked first while each names the other (`duel_pairs`)
    duel: u32,
    /// the lobby of the race it last reported, and when (one report a race is logged); the same of its last results
    /// (RACERES, sent with the report)
    reported: Option<(u32, f64)>,
    resulted: Option<(u32, f64)>,
    /// its links after the last tick (a HELLO of another start of the game empties them)
    links: Vec<Link>,
    /// how its car moves (watch.rs)
    watch: Watch,
    /// its place in `order`
    at: u32,
    /// the keyphrase its page plays with (nfsu2-claims' owner: its hello to the switch named it), if any
    owner: Option<Arc<str>>,
    /// one id a friendship of its owner's (`friend_id`), each friend once: a friend of its room is linked with it
    /// first, as a crew-mate is (`crew_pairs`)
    friend_ids: Vec<u32>,
    checked: u64,
    /// when its owner's claim last heard it played (`Claims::seen`, at most every SEEN_EVERY_MS)
    seen_claim: f64,
}

impl Player {
    fn key(&self) -> u64 {
        key(self.vip, self.port)
    }
}

/// What a browser's page blocks (its hello's "block": `Hub::blocks`): names as their Online form in lower case
/// (nfsu2-claims `canonical`: "Bob", "BOB" and "bob " are one name), and the keyphrases that hold them as claimed
/// names, as of the claims' `generation` in `checked`.  A player is blocked by its name, or by its page's keyphrase: one
/// that changes its claimed name stays blocked while its old name is still its own (GRACE_DAYS).
struct Block {
    names: Vec<String>,
    owners: Vec<Arc<str>>,
    checked: u64,
}

impl Block {
    fn new(names: &[String]) -> Block {
        let mut names: Vec<String> = names.iter().filter_map(|n| nfsu2_claims::canonical(n)).collect();
        names.sort_unstable();
        names.dedup();
        Block { names, owners: Vec::new(), checked: u64::MAX }
    }

    /// the keyphrases that hold its names now, when the claims changed since it last looked
    fn owners_of(&mut self, claims: Option<&nfsu2_claims::Claims>, now: f64) {
        let Some(c) = claims.filter(|c| c.generation() != self.checked) else { return };
        self.checked = c.generation();
        self.owners = self.names.iter().filter_map(|n| c.holder(n, now as i64)).collect();
    }

    /// whether it blocks player p
    fn has(&self, p: &Player) -> bool {
        (!p.canon.is_empty() && self.names.contains(&p.canon)) || p.owner.as_ref().is_some_and(|o| self.owners.contains(o))
    }
}

/// a player's endpoint, its virtual address and port, in one number
fn key(vip: u32, port: u16) -> u64 {
    (vip as u64) << 16 | port as u64
}

/// a HELLO's crews as the hub keeps them: each one once (a page may name a crew twice), in its order, 0 after them
fn distinct(crews: [u32; wire::CREWS]) -> [u32; wire::CREWS] {
    let (mut out, mut n) = ([0u32; wire::CREWS], 0);
    for c in crews {
        if c != 0 && !out[..n].contains(&c) {
            out[n] = c;
            n += 1;
        }
    }
    out
}

/// a player's crews: the nonzero ones of its `crews`
fn crews_of(p: &Player) -> impl Iterator<Item = u32> + '_ {
    p.crews.iter().copied().take_while(|&c| c != 0)
}

/// The id of the friendship of keyphrases a and b (nfsu2-claims' owners), the same both ways: each of the two players
/// has it only when each one's friends list names the other, so the two are a crew of two (`crew_pairs`).  An FNV-1a
/// of the two, as a crew's word is one (a crew sharing it would only link the two first too); never 0.
fn friend_id(a: &str, b: &str) -> u32 {
    let (lo, hi) = if a <= b { (a, b) } else { (b, a) };
    let mut h: u32 = 0x811C_9DC5;
    for &c in lo.as_bytes().iter().chain(b"\0").chain(hi.as_bytes()) {
        h = (h ^ c as u32).wrapping_mul(0x0100_0193);
    }
    h.max(1)
}

/// a player's friendships (`friend_id`): of its owner with each of `friends` (theirs), none without an owner
fn friend_ids(owner: Option<&Arc<str>>, friends: Option<&Vec<Arc<str>>>) -> Vec<u32> {
    let (Some(o), Some(f)) = (owner, friends) else { return Vec::new() };
    let mut ids: Vec<u32> = f.iter().filter(|x| ***x != **o).map(|x| friend_id(o, x)).collect();
    ids.sort_unstable();
    ids.dedup();
    ids
}

/// Where a friend is (`Hub::whereabouts`): in free roam, in this room (rc_room_hash; 0: the room of everyone), whose
/// name its page told the hub (`Hub::room_name`; None: not told)
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Whereabouts {
    pub room: u32,
    pub room_name: Option<String>,
}

/// the nearest integer, a half up (to +infinity, where `f64::round` takes it away from 0)
fn js_round(x: f64) -> f64 {
    let c = x.ceil();
    if c - 0.5 <= x { c } else { c - 1.0 }
}

/// the options: a value that is not a positive number keeps its default
fn sanitize(o: RoamOptions) -> RoamOptions {
    let d = RoamOptions::default();
    let n = |v: usize, d: usize| if v > 0 { v } else { d };
    let f = |v: f64, d: f64| if v.is_finite() && v > 0.0 { v } else { d };
    RoamOptions {
        // (roam_core reads a list of RC_MAX_PEERS at most)
        degree: n(o.degree, d.degree).min(wire::LINKS_MAX),
        tick_ms: if o.tick_ms > 0 { o.tick_ms } else { d.tick_ms },
        stale_ms: f(o.stale_ms, d.stale_ms),
        gone_ms: f(o.gone_ms, d.gone_ms),
        keep: f(o.keep, d.keep),
        hold_ms: f(o.hold_ms, d.hold_ms),
        max_per_vip: n(o.max_per_vip, d.max_per_vip),
        max_players: n(o.max_players, d.max_players),
        exact_room: n(o.exact_room, d.exact_room),
        min_degree: n(o.min_degree, d.min_degree),
        busy_high: f(o.busy_high, d.busy_high),
        busy_low: f(o.busy_low, d.busy_low),
        cap_down_ms: f(o.cap_down_ms, d.cap_down_ms),
        cap_up_ms: f(o.cap_up_ms, d.cap_up_ms),
        ask_cap: n(o.ask_cap, d.ask_cap),
        soon_ms: f(o.soon_ms, d.soon_ms),
        look_ms: f(o.look_ms, d.look_ms),
        away_m: f(o.away_m, d.away_m),
        view_m: f(o.view_m, d.view_m),
        level_slack_m: f(o.level_slack_m, d.level_slack_m),
        level_grade: f(o.level_grade, d.level_grade),
        level_k: f(o.level_k, d.level_k),
        // (one answer is MAPD_PARTS datagrams at most)
        map_max: n(o.map_max, d.map_max).min(MAPD_MAX),
        map_gap_ms: f(o.map_gap_ms, d.map_gap_ms),
        map_fresh_ms: f(o.map_fresh_ms, d.map_fresh_ms),
        near_gap_ms: f(o.near_gap_ms, d.near_gap_ms),
        link_threads: n(o.link_threads, d.link_threads).min(64),
        deny: o.deny,
    }
}

#[derive(Default)]
struct Stats {
    frames: u64,
    bad: u64,
    refused: u64,
    lists: u64,
    ticks: u64,
    tick_ms_max: f64,
    busy: f64,
    cap_cuts: u64,
    /// links outruns between players needed made before any other (`duel_pairs`), at the last tick
    duel_links: u64,
    /// links a race lobby needed made before the others (`lobby_pairs`), at the last tick
    lobby_links: u64,
    /// links made for crews before the others (`crew_pairs`), and the crews too large for that (CREW_GROUP_MAX
    /// players in one room), at the last tick
    crew_links: u64,
    crew_skips: u64,
    /// players' race reports logged
    race_reports: u64,
    /// `[cheat]` lines logged
    cheat_lines: u64,
    /// the minimap's answers (NEARD) sent
    nears: u64,
}

/// one pair of a room weighed (every pair of a room of at most `exact_room` players): since when it is linked; a and b
/// are places in the room's list, a < b (its place in the order is in `Scratch::order`)
struct Pair {
    since: f64,
    a: u32,
    b: u32,
}

/// What one tick needs, kept from tick to tick so that a tick allocates (almost) nothing.
#[derive(Default)]
struct Scratch {
    room_of: HashMap<u32, usize>,
    rooms: Vec<Vec<u32>>,
    /// each slot's new links
    fresh: Vec<Vec<Link>>,
    /// each slot's place in the room being linked (NONE: not in it)
    local: Vec<u32>,
    // by the place in the room's list: where each one is, and where the links weigh it (`prepare`): soon_ms on,
    // look_ms on, and its view point
    pos: Vec<[f64; 3]>,
    soon: Vec<Place>,
    ahead: Vec<Place>,
    view: Vec<Place>,
    deg: Vec<usize>,
    /// each place's crew links at most (`crew_cap`)
    ccap: Vec<u32>,
    mark: Vec<u32>,
    since: Vec<f64>,
    pairs: Vec<Pair>,
    /// each pair's place in the order (`order_key` of its d) and its index in `pairs`, and room to sort them
    order: Vec<(u64, u32)>,
    sorting: Vec<(u64, u32)>,
    grid: Grid,
    heap: Heap,
    near: Vec<Near>,
    /// the same over the places ahead
    grid_a: Grid,
    near_a: Vec<Near>,
    /// the walks from the view points over the places soon (`grid`)
    near_v: Vec<Near>,
    /// each Near's vectors of what it found, kept for the next tick
    found: Vec<Found>,
    /// the outruns' pairs (`duel_pairs`), places in the room, the lower first; the room's players who name another, as
    /// (their id, their place), sorted
    dpairs: Vec<(u32, u32)>,
    dids: Vec<(u32, u32)>,
    /// the race lobbies' pairs (`lobby_pairs`), places in the room, the host first; the room's ids to places; the
    /// players asking to join a lobby (its id, their place); the players near a lobby's start
    lpairs: Vec<(u32, u32)>,
    lids: HashMap<u32, u32>,
    ljoin: Vec<(u32, u32)>,
    lnear: Vec<(f64, u32)>,
    /// each lobby's host and its players there, as places (`lobby_pairs`)
    lmembers: Vec<(u32, u32)>,
    /// the pairs kept apart (`block_pairs`): places in the room, the lower first, sorted; each place's others as
    /// `bnext[bstart[i]..bstart[i + 1]]` (bstart empty: none in the room); the room's blockers, as places
    bpairs: Vec<(u32, u32)>,
    bstart: Vec<u32>,
    bnext: Vec<u32>,
    bfrom: Vec<u32>,
    /// the crews' pairs (`crew_pairs`): their place in the crews' order and the two places in the room, the lower
    /// first; the room's players in crews as (crew, place); a place's links of the last tick, stamped (`cmark`, and
    /// since when, `csince`); the links each place got for its crews (`ccount`)
    cpairs: Vec<(u64, u32, u32)>,
    cmembers: Vec<(u32, u32)>,
    cmark: Vec<u32>,
    csince: Vec<f64>,
    ccount: Vec<u32>,
    /// each place's links made first, the lobbies' and the crews' (the first of its new links: `link_every_pair`)
    npre: Vec<u32>,
    /// a list's crew marks, one a link (`lists`)
    marks: Vec<u8>,
    /// a large room's links of the last tick, sorted (`link_many`)
    olds: Vec<Cand>,
    /// each place's links of the last tick as places in the room: `olinks[ostart[i]..ostart[i + 1]]`, and since when
    /// each one is (`osince`, by the same index)
    olinks: Vec<u32>,
    osince: Vec<f64>,
    ostart: Vec<u32>,
    /// each place's walks read ahead (`Walks`): walk w of place i at `pre[(3 i + w) n..][..pre_n[3 i + w]]`, and how
    /// many of them the tick took (`pre_at`)
    pre: Vec<(f64, u32)>,
    pre_n: Vec<u16>,
    pre_at: Vec<u16>,
    /// how far each walk is read ahead (`Walks::want`), and by slot how far its walks went at the last tick, plus 1 (0:
    /// not known)
    want: Vec<u16>,
    walk_used: Vec<[u16; 3]>,
    /// each place's new links as places (`flinks[i * stride..][..fcount[i]]`)
    fcount: Vec<u32>,
    flinks: Vec<u32>,
    /// the block the lists are cut from (`lists`)
    buf: BytesMut,
}

/// The bytes of a block of lists: what a list waiting in a slow player's queue keeps at most
const LIST_BLOCK: usize = 4096;

/// what the links of a room are weighed by, and the threads its walks are read on (`RoamOptions::link_threads`)
struct Weigh {
    now: f64,
    hold_ms: f64,
    keep: f64,
    threads: usize,
    /// what a gap in height between two roads weighs (RoamOptions level_*)
    level: Level,
}

/// how far apart places i and j of a room count: the nearest of the two soon_ms on, the two look_ms on, and either
/// one's view point to the other soon_ms on (each weighed with its gap in height, `weigh`)
#[inline]
fn apart(soon: &[Place], ahead: &[Place], view: &[Place], i: usize, j: usize, l: &Level) -> f64 {
    weigh(&soon[i], &soon[j], l).min(weigh(&ahead[i], &ahead[j], l)).min(weigh(&view[i], &soon[j], l)).min(weigh(&view[j], &soon[i], l))
}

/// The hub's logic, without its timer (`RoamHub` gives it one).
pub struct Hub {
    o: RoamOptions,
    now: Box<dyn Fn() -> f64 + Send>,
    busy: Box<dyn FnMut() -> f64 + Send>,
    log: Box<dyn Fn(&str) + Send>,
    send: Box<dyn Fn(u32, Bytes) -> bool + Send>,
    id: u32,
    seq: u16,
    /// the cap on every player's links (its degree at most), and when it last changed
    cap: usize,
    cap_at: f64,
    /// how busy the hub's own thread was over the last second, 0 to 1 (`set_thread_busy`)
    thread_busy: f64,
    st: Stats,
    slots: Vec<Option<Player>>,
    free: Vec<u32>,
    /// the slots of players gone that others' links may still name (`drop_slots`), and by slot whether it is one
    dead: Vec<u32>,
    dead_at: Vec<bool>,
    /// the slots in the order the players came (NONE where one went)
    order: Vec<u32>,
    holes: usize,
    by_key: HashMap<u64, u32>,
    /// per browser (virtual address), the players its page blocks (`blocks`), until it leaves the switch
    blocks: HashMap<u32, Block>,
    /// per browser, its page's name on the switch (`page_name`), until it leaves the switch
    pages: HashMap<u32, Page>,
    /// per browser, its keyphrase's friends (their owners: `friends`), and the name of the room its page plays in
    /// (`room_name`: for its friends' Join), until it leaves the switch
    friends: HashMap<u32, Vec<Arc<str>>>,
    room_names: HashMap<u32, String>,
    /// the players in free roam by their keyphrase (made at each tick): `whereabouts`
    owners_at: HashMap<Arc<str>, u32>,
    /// the pairs of players kept apart at the last tick (`block_pairs`), as slots, the lower first, sorted: the world
    /// map and the lobby list read them between ticks
    apart: Vec<(u32, u32)>,
    /// the endpoints of each virtual address (`max_per_vip`)
    per_vip: HashMap<u32, u32>,
    /// the players whose last HELLO named a crew: with none, a tick does not look for crews at all
    crewed: usize,
    /// the cheat watches of players gone without a BYE, by endpoint, for WATCH_KEEP_MS (at most max_players of them)
    kept: HashMap<u64, KeptWatch>,
    /// the races' results as the players' games showed them, until each race is judged (results.rs), and the lobbies
    /// whose players' results are taken, by their ids
    results: Results,
    /// the lobbies' races' honest times, an edited car's compared with them (`edited_car_line`)
    times: Times,
    joined: HashMap<u32, Joined>,
    /// each browser's lobbies in `joined`, oldest first; when `joined` was last pruned
    joined_by_vip: HashMap<u32, Vec<u32>>,
    joined_pruned: f64,
    /// when each virtual address's next world map is answered at the soonest (map_gap_ms), kept past its players: a BYE and a HELLO (or
    /// another port) make a new player, and when a player's own time was the limit, a new player's first answer came
    /// at once (the second security audit, 2026-10-22: a map walks every player on the hub's one thread, and a few
    /// clients asking that way kept it busy); dropped at each tick once past
    map_at: HashMap<u32, f64>,
    /// MAPD: each browser's map as it holds it (`MapHeld`), and when its next race lobbies go
    maps: HashMap<u32, MapHeld>,
    lobbies_at: HashMap<u32, f64>,
    /// MAPD: each room's players the map shows, by slot, where they are at the time with it (made at the first
    /// question of a tick)
    map_cands: HashMap<u32, (f64, Vec<MapCand>)>,
    /// NEARD (the minimap): as map_at and maps, for each browser's minimap; and every room's players the maps show by
    /// room and cell (`near_grid_of`), made at the first minimap question of a tick
    near_at: HashMap<u32, f64>,
    nears: HashMap<u32, MapHeld>,
    near_grid: Option<NearGrid>,
    /// the grids made so far (`NearGrid::nth`)
    near_gens: u64,
    /// a question's players within its radius (distance's bits, slot): kept from question to question
    near_found: Vec<(u32, u32)>,
    /// by slot, the stamp of the question whose candidates may not be that player (the asker and its links: one look
    /// each, where a search of the links was most of picking them in a crowd)
    near_mark: Vec<u32>,
    near_stamp: u32,
    /// an answer's nearest ones (distance's bits, slot, place in the grid) and its players (kept from answer to answer)
    near_picks: Vec<(u32, u32, u32)>,
    near_list: Vec<MapCand>,
    s: Scratch,
    /// the claimed names (nfsu2-claims): a player under another keyphrase's name plays alone; None: any name is anyone's
    claims: Option<Arc<nfsu2_claims::Claims>>,
}

impl Hub {
    /// `now`: the clock in ms (nfsu2_util::now_ms() in the server); `busy`: 0 to 1, how busy the server was since
    /// the last tick; `log`: a console line; `send(vip, frame)`: through the switch (true: it went); `id`: the hub's
    /// own sender id (random in the server)
    pub fn new(
        o: RoamOptions,
        now: Box<dyn Fn() -> f64 + Send>,
        busy: Box<dyn FnMut() -> f64 + Send>,
        log: Box<dyn Fn(&str) + Send>,
        send: Box<dyn Fn(u32, Bytes) -> bool + Send>,
        id: u32,
    ) -> Self {
        let o = sanitize(o);
        Hub {
            cap: o.degree,
            cap_at: f64::NEG_INFINITY,
            thread_busy: 0.0,
            o,
            now,
            busy,
            log,
            send,
            id,
            seq: 0,
            st: Stats::default(),
            slots: Vec::new(),
            free: Vec::new(),
            dead: Vec::new(),
            dead_at: Vec::new(),
            order: Vec::new(),
            holes: 0,
            by_key: HashMap::new(),
            blocks: HashMap::new(),
            pages: HashMap::new(),
            friends: HashMap::new(),
            room_names: HashMap::new(),
            owners_at: HashMap::new(),
            apart: Vec::new(),
            per_vip: HashMap::new(),
            crewed: 0,
            kept: HashMap::new(),
            results: Results::default(),
            times: Times::default(),
            joined: HashMap::new(),
            joined_by_vip: HashMap::new(),
            joined_pruned: 0.0,
            map_at: HashMap::new(),
            maps: HashMap::new(),
            lobbies_at: HashMap::new(),
            map_cands: HashMap::new(),
            near_at: HashMap::new(),
            nears: HashMap::new(),
            near_grid: None,
            near_gens: 0,
            near_found: Vec::new(),
            near_mark: Vec::new(),
            near_stamp: 0,
            near_picks: Vec::new(),
            near_list: Vec::new(),
            s: Scratch::default(),
            claims: None,
        }
    }

    /// the claimed names its players' names are checked against (the player's page's keyphrase: `frame_from`)
    pub fn set_claims(&mut self, claims: Option<Arc<nfsu2_claims::Claims>>) {
        self.claims = claims;
    }

    /// a player's name against the claims and the names the operator took away (`RoamOptions::deny`), when its name,
    /// its owner, the claims or those names changed since it was last looked at: another keyphrase's name, or one taken
    /// away, makes it an impostor (the console says so once); its own claim hears it plays
    fn check_claim(&mut self, slot: u32, now: f64) {
        if self.claims.is_none() && self.o.deny.is_none() {
            return;
        }
        // both generations in one number, never 0 (0: not looked at yet)
        let cg = self.claims.as_ref().map_or(0, |c| c.generation());
        let dg = self.o.deny.as_ref().map_or(0, |d| d.generation());
        let g = (cg << 32 | dg & 0xFFFF_FFFF).wrapping_add(1);
        let Some(Some(p)) = self.slots.get_mut(slot as usize) else { return };
        if p.checked != g {
            p.checked = g;
            let was = p.impostor;
            let denied = self.o.deny.as_ref().is_some_and(|d| d.has(&p.name));
            p.impostor = denied || self.claims.as_ref().is_some_and(|c| !c.may_use(&p.name, p.owner.as_deref(), now as i64));
            if p.impostor && !was {
                (self.log)(&format!(
                    "[roam] {} plays as \"{}\", {}: no links",
                    nfsu2_util::vip_str(p.vip),
                    js::printable(&p.name, 32),
                    if denied { "a name the operator took away" } else { "a name another keyphrase claimed" }
                ));
            }
        }
        if let (Some(c), Some(o)) = (&self.claims, p.owner.as_deref().filter(|_| !p.impostor && now - p.seen_claim >= SEEN_EVERY_MS)) {
            p.seen_claim = now;
            c.seen(&p.name, o, now as i64);
        }
    }

    /// the switch's way to its browsers
    pub fn attach_switch(&mut self, send: Box<dyn Fn(u32, Bytes) -> bool + Send>) {
        self.send = send;
    }

    /// its options, each one that was not a positive number at its default
    pub fn options(&self) -> &RoamOptions {
        &self.o
    }

    fn player(&self, slot: u32) -> Option<&Player> {
        self.slots.get(slot as usize)?.as_ref()
    }

    /// the minimap grid's `live` of the player in `slot` as it is now, if the grid has it: after each frame of its
    /// (the only place its position, status, room, id and claim change) and once it is gone
    fn shown_sync(&mut self, slot: u32) {
        let Some(k) = self.near_grid.as_ref().and_then(|g| g.pos.get(slot as usize).copied()).filter(|&k| k != u32::MAX) else {
            return;
        };
        let s = self.player(slot).map(Shown::of).unwrap_or_default();
        if let Some(x) = self.near_grid.as_mut().and_then(|g| g.live.get_mut(k as usize)) {
            *x = s;
        }
    }

    /// the players in the order they came
    fn players(&self) -> impl Iterator<Item = &Player> + '_ {
        self.order.iter().filter_map(|&s| self.slots.get(s as usize)?.as_ref())
    }

    fn insert(&mut self, mut p: Player) -> u32 {
        let k = p.key();
        // its watch, if it was here not long ago (of another start of its game: where its car was, and its clock's
        // epochs, start again)
        if let Some(w) = self.kept.remove(&k).filter(|w| (self.now)() - w.at < WATCH_KEEP_MS) {
            p.watch = w.watch;
            if w.id != p.id {
                p.watch.reset();
            }
        }
        let slot = match self.free.pop() {
            Some(s) => s,
            None => {
                self.slots.push(None);
                (self.slots.len() - 1) as u32
            }
        };
        *self.per_vip.entry(p.vip).or_insert(0) += 1;
        self.order.push(slot);
        self.slots[slot as usize] = Some(Player { at: (self.order.len() - 1) as u32, ..p });
        self.by_key.insert(k, slot);
        slot
    }

    /// out of the slots and the order (not out of the links: `drop_slots` does that); false if it was empty.  `keep`:
    /// gone without its BYE, its cheat watch is kept for a while (else summed up now)
    fn remove(&mut self, slot: u32, keep: bool) -> bool {
        let Some(mut p) = self.slots.get_mut(slot as usize).and_then(Option::take) else { return false };
        self.shown_sync(slot);
        let now = (self.now)();
        if keep && self.kept.len() < self.o.max_players {
            let w = KeptWatch { watch: std::mem::replace(&mut p.watch, Watch::new(now)), id: p.id, name: p.name.clone(), at: now };
            self.kept.insert(p.key(), w);
        } else if let Some(line) = p.watch.summary(p.vip, &p.name, now, true) {
            self.st.cheat_lines += 1;
            (self.log)(&line);
        }
        self.by_key.remove(&p.key());
        self.crewed -= (p.crews[0] != 0) as usize;
        if let Some(n) = self.per_vip.get_mut(&p.vip) {
            *n -= 1;
            if *n == 0 {
                self.per_vip.remove(&p.vip);
            }
        }
        if let Some(o) = self.order.get_mut(p.at as usize) {
            *o = NONE;
        }
        self.holes += 1;
        if self.holes > 64 && self.holes * 2 > self.order.len() {
            self.order.retain(|&s| s != NONE);
            self.holes = 0;
            for (at, &s) in self.order.iter().enumerate() {
                if let Some(Some(q)) = self.slots.get_mut(s as usize) {
                    q.at = at as u32;
                }
            }
        }
        true
    }

    /// a player that said BYE: out of the players and out of every link
    fn drop_key(&mut self, k: u64) {
        let Some(&slot) = self.by_key.get(&k) else { return };
        self.drop_slots(&[slot], false);
    }

    /// Players gone at once: the same as one after another (each goes out of the order and every link alike).
    ///
    /// Out of the others' links they go later, many at once (`purge`): taking one out walks every player's links
    /// (64,000 of 2000 players), and a client may send a HELLO and a BYE thousands of times a second.  Until then its
    /// slot is `dead`, links to it count for nothing (`live`), and it is not used again, so that no link can name a
    /// newcomer in its place.  `keep`: gone without a BYE (their cheat watches are kept a while)
    fn drop_slots(&mut self, slots: &[u32], keep: bool) {
        self.dead_at.resize(self.slots.len(), false);
        for &s in slots {
            if self.remove(s, keep) {
                self.dead_at[s as usize] = true;
                self.dead.push(s);
            }
        }
        // a purge walks every link: once for many players gone, at least an eighth of the players
        if self.dead.len() >= 64.max(self.by_key.len() / 8) {
            self.purge();
        }
    }

    /// the players gone out of the others' links, and their slots free again
    fn purge(&mut self) {
        if self.dead.is_empty() {
            return;
        }
        let dead = &self.dead_at;
        for q in self.slots.iter_mut().flatten() {
            q.links.retain(|l| !dead.get(l.slot as usize).copied().unwrap_or(false));
        }
        for s in self.dead.drain(..) {
            self.dead_at[s as usize] = false;
            self.free.push(s);
        }
    }

    /// a link to a player that is still there (not one gone and not purged yet)
    fn live(&self, l: &Link) -> bool {
        !self.dead_at.get(l.slot as usize).copied().unwrap_or(false)
    }

    /// a frame from browser `from` to the hub's address (the switch has written the true source vip)
    pub fn frame(&mut self, from: u32, data: &[u8]) {
        self.frame_from(from, data, None);
    }

    /// a frame from the browser at `from`, whose page plays with keyphrase `owner` (`FrameCtx::owner`)
    pub fn frame_from(&mut self, from: u32, data: &[u8], owner: Option<&Arc<str>>) {
        self.st.frames += 1;
        if data.len() < F_DATA + wire::HDR + wire::CRC || data[0] != KIND_UDP {
            self.st.bad += 1;
            return;
        }
        let (sport, dport) = (u16::from_be_bytes([data[12], data[13]]), u16::from_be_bytes([data[14], data[15]]));
        let Some((_, sender, m)) = wire::decode(&data[F_DATA..]).filter(|_| sport != 0) else {
            self.st.bad += 1;
            return;
        };
        let k = key(from, sport);
        let found = self.by_key.get(&k).copied();
        if let Msg::Bye = m {
            if found.is_some() {
                self.drop_key(k);
            }
            return;
        }
        // (a new player, or a HELLO with a new name: its name against its page's)
        let mut renamed = found.is_none();
        let slot = match found {
            Some(s) => s,
            None => {
                // a player starts with its HELLO (the room)
                let Msg::Hello { status, room, name, .. } = m else { return };
                let mine = self.per_vip.get(&from).copied().unwrap_or(0) as usize;
                if mine >= self.o.max_per_vip || self.by_key.len() >= self.o.max_players {
                    self.st.refused += 1;
                    return;
                }
                self.insert(Player {
                    vip: from,
                    port: sport,
                    hub_port: dport,
                    id: sender,
                    room,
                    status,
                    name: String::from_utf8_lossy(name).into_owned(),
                    canon: nfsu2_claims::canonical(&String::from_utf8_lossy(name)).unwrap_or_default(),
                    degree: 0,
                    p: None,
                    v: [0.0; 3],
                    heading: 0,
                    slope: 0,
                    pos_at: 0.0,
                    stay: [0.0; 2],
                    stay_at: f64::INFINITY,
                    seen: 0.0,
                    lobbies: false,
                    marks: false,
                    crews: [0; wire::CREWS],
                    lobby: None,
                    join: None,
                    duel: 0,
                    reported: None,
                    resulted: None,
                    links: Vec::new(),
                    watch: Watch::new((self.now)()),
                    at: 0,
                    owner: owner.cloned(),
                    friend_ids: friend_ids(owner, self.friends.get(&from)),
                    impostor: false,
                    checked: 0,
                    seen_claim: f64::NEG_INFINITY,
                })
            }
        };
        let now = (self.now)();
        let degree = self.o.degree;
        let Some(Some(p)) = self.slots.get_mut(slot as usize) else { return };
        p.seen = now;
        p.hub_port = dport;
        let mut ask = None;
        let mut ask_near = None;
        let mut report = None;
        let mut edited = None;
        let mut seen = [None, None];
        let mut res = None;
        let mut heard = None;
        // (in a crew before this HELLO, and after it)
        let mut crewed = None;
        // (a HELLO: its name against the claims)
        let mut check = false;
        match m {
            Msg::Hello { sender, core, status, room, name, lobbies, marks, crews, links, duel } => {
                // another start of the game on the same endpoint (where its car was, and its clock's epochs, start again)
                if p.id != sender {
                    p.links.clear();
                    p.watch.reset();
                }
                p.id = sender;
                p.room = room;
                p.status = status;
                if !matches!(status, 2 | 5) {
                    p.watch.reset();
                }
                if p.name.as_bytes() != name {
                    p.name.clear();
                    p.name.extend(name.iter().map(|&c| c as char));
                    p.canon = nfsu2_claims::canonical(&p.name).unwrap_or_default();
                    p.checked = 0;
                    renamed = true;
                }
                if p.owner.as_ref() != owner {
                    p.owner = owner.cloned();
                    p.checked = 0;
                    p.friend_ids = friend_ids(owner, self.friends.get(&from));
                }
                check = true;
                // the links it wants (core 10), the hub's most at most; a core before reads lists of 32 at most
                p.degree = if core >= 10 && links > 0 { (links as usize).min(degree) } else { degree.min(wire::LINKS_V9) };
                p.lobbies = lobbies;
                p.marks = marks;
                p.duel = duel;
                let was = p.crews[0] != 0;
                p.crews = distinct(crews);
                crewed = Some((was, p.crews[0] != 0));
            }
            Msg::Snap { sender, status, p: pos, v, heading, slope, t_us, clock } if sender == p.id => {
                let (dx, dy) = (pos[0] - p.stay[0], pos[1] - p.stay[1]);
                if p.p.is_none() || dx * dx + dy * dy > STAY_M * STAY_M || !matches!(status, 2 | 5) {
                    (p.stay, p.stay_at) = ([pos[0], pos[1]], now);
                }
                p.p = Some(pos);
                p.v = v;
                p.heading = heading;
                p.slope = slope;
                p.pos_at = p.seen;
                p.status = status;
                if matches!(status, 2 | 5) {
                    seen = p.watch.snap(now, pos, t_us, clock);
                } else {
                    p.watch.reset();
                }
            }
            Msg::MapReq { req, max, base, flags } if sender == p.id => ask = Some((req, max, base, flags)),
            Msg::NearReq { req, max, base, radius } if sender == p.id => ask_near = Some((req, max, base, radius)),
            Msg::Lobby { op, to: 0, lobby, race, x, y, n, max, state, ids } if sender == p.id => {
                match op {
                    wire::LOBBY_ADVERT => p.lobby = Some((HubLobby { id: lobby, race, x, y, n, max, state, ids }, now)),
                    wire::LOBBY_CLOSE if p.lobby.is_some_and(|(l, _)| l.id == lobby) => p.lobby = None,
                    wire::LOBBY_JOIN => p.join = Some((lobby, now)),
                    _ => {}
                }
                if matches!(op, wire::LOBBY_ADVERT | wire::LOBBY_JOIN) {
                    heard = Some((op, lobby, race, p.id, p.vip));
                }
            }
            // one report a race (its lobby), and none within REPORT_GAP_MS of the player's last; its results the same
            Msg::RaceRep(r) if sender == p.id && !p.reported.is_some_and(|(l, t)| l == r.lobby || now - t < REPORT_GAP_MS) => {
                p.reported = Some((r.lobby, now));
                // (an edited car's time against the honest ones before this report: its own is none of them)
                let (line, note) = edited_car_line(&format!("{} \"{}\"", vip_str(p.vip), p.name), &r, self.times.compare(&r).as_ref());
                self.times.add(&r);
                report = Some(race_line(&p.name, &r) + &note);
                edited = line;
            }
            Msg::RaceRes(r) if sender == p.id && !p.resulted.is_some_and(|(l, t)| l == r.lobby || now - t < REPORT_GAP_MS) => {
                p.resulted = Some((r.lobby, now));
                res = Some((p.id, r));
            }
            _ => {}
        }
        if let Some((op, lobby, race, id, vip)) = heard {
            self.lobby_heard(op, lobby, race, id, vip, now);
        }
        if let Some((was, is)) = crewed {
            self.crewed = self.crewed + is as usize - was as usize;
        }
        if check {
            self.check_claim(slot, now);
        }
        self.shown_sync(slot);
        if renamed {
            self.name_check(slot);
        }
        if let Some(line) = report {
            self.st.race_reports += 1;
            (self.log)(&line);
        }
        if let Some(line) = edited {
            self.st.cheat_lines += 1;
            (self.log)(&line);
        }
        for s in seen.into_iter().flatten() {
            self.cheat(slot, s, now);
        }
        // a race's results from one of its lobby's players, naming only them (a racer not of it is nobody: no line
        // names it); none of a lobby the hub did not see
        if let Some((by, mut r)) = res
            && let Some(ids) = self.joined.get(&r.lobby).filter(|j| j.race == r.race && j.ids.contains(&by)).map(|j| j.ids.clone())
        {
            for x in &mut r.rows[..r.n] {
                if !ids.contains(&x.id) {
                    x.id = 0;
                }
            }
            let mut results = std::mem::take(&mut self.results);
            results.add(now, by, r, &|id| self.who(id));
            self.results = results;
        }
        if let Some((req, max, base, flags)) = ask {
            self.mapd(slot, req, max, base, flags & wire::MAPREQ_PARKED != 0, now);
        }
        if let Some((req, max, base, radius)) = ask_near {
            self.neard(slot, req, max, base, radius, now);
        }
    }

    /// A lobby's advert (from its host) or a player's own ask to join it: its players, for its races' results
    /// (`Joined`).  An advert of a lobby another browser hosts, and an ask for a lobby the hub did not see, count for
    /// nothing.
    fn lobby_heard(&mut self, op: u8, lobby: u32, race: u32, id: u32, vip: u32, now: f64) {
        if let Some(j) = self.joined.get_mut(&lobby) {
            if j.race == race && (op != wire::LOBBY_ADVERT || j.vip == vip) {
                if !j.ids.contains(&id) && j.ids.len() < JOINED_IDS {
                    j.ids.push(id);
                }
                j.at = now;
            }
            return;
        }
        if op != wire::LOBBY_ADVERT {
            return;
        }
        // (a browser's newest lobbies only)
        let mine = self.joined_by_vip.entry(vip).or_default();
        if mine.len() >= JOINED_PER_VIP {
            let old = mine.remove(0);
            self.joined.remove(&old);
        }
        mine.push(lobby);
        self.joined.insert(lobby, Joined { race, vip, ids: vec![id], at: now });
    }

    /// What a player's SNAP showed of its car (watch.rs) as a `[cheat]` line, while it has lines: who was next to where
    /// its car came to (the other players of its room in free roam, each where its last SNAP and speed have it now),
    /// and a race lobby that starts there
    fn cheat(&mut self, slot: u32, seen: Seen, now: f64) {
        let Some(p) = self.player(slot) else { return };
        let (room, vip) = (p.room, p.vip);
        let (mut near, mut all, mut lobby) = (Vec::new(), 0, None);
        if let Seen::Jump { to, .. } | Seen::Fast { to, .. } = seen {
            // (not its own other tab: the same browser's address)
            for &q in &self.order {
                let Some(o) = self.player(q).filter(|o| o.vip != vip && o.room == room) else { continue };
                if let Some(at) = o.p.filter(|_| matches!(o.status, 2 | 5) && now - o.pos_at <= NEAR_FRESH_MS) {
                    let k = (now - o.pos_at) / 1000.0;
                    let there = [at[0] + o.v[0] * k, at[1] + o.v[1] * k, at[2] + o.v[2] * k];
                    let d = watch::distance(&there, &to);
                    if d < watch::NEAR_M {
                        all += 1;
                        near.push((d, o.name.clone(), o.vip));
                    }
                }
                if lobby.is_none()
                    && let Some((l, t)) = o.lobby
                    && now - t < LOBBY_STALE_MS
                    && (l.x as f64 - to[0]).hypot(l.y as f64 - to[1]) < LOBBY_LAND_M
                {
                    lobby = Some(o.name.clone());
                }
            }
            near.sort_by(|a, b| a.0.total_cmp(&b.0));
            near.truncate(NEAR_NAMED);
        }
        let Some(Some(p)) = self.slots.get_mut(slot as usize) else { return };
        if matches!(seen, Seen::Jump { .. }) && all > 0 {
            p.watch.landed_near();
        }
        let Some(last) = p.watch.line(now) else { return };
        let near = watch::Near { players: &near, all, lobby: lobby.as_deref() };
        let line = watch::line(p.vip, &p.name, &seen, &near, p.watch.jumps, last);
        self.st.cheat_lines += 1;
        (self.log)(&line);
    }

    /// a player by its game's id, as a line names it: its address and name (one gone a while ago by its kept watch)
    fn who(&self, id: u32) -> Option<String> {
        if let Some(p) = self.players().find(|p| p.id == id) {
            return Some(format!("{} \"{}\"", nfsu2_util::vip_str(p.vip), p.name));
        }
        let (k, w) = self.kept.iter().find(|(_, w)| w.id == id)?;
        Some(format!("{} \"{}\"", nfsu2_util::vip_str((k >> 16) as u32), w.name))
    }

    /// The world map's answer to the player in `slot` (roam_core.h RC_MSG_MAPD): the players of its room in free roam
    /// (or paused there) with a position younger than map_fresh_ms, past itself and its links (`map_room`), map_max of
    /// them at most (and the most it asks for; by slot, the nearest when there are more), as what changed since its
    /// answer `base` the hub sent (from nothing when the hub has no such answer): a player come or moved off where the page has it now (`MAP_TOL_M`, `MAP_TOL_H`;
    /// the page moves each marker on at the velocity it was given) is placed, a player gone goes.  One answer to a
    /// browser every map_gap_ms at most, and its race lobbies with it every MAP_LOBBIES_GAP_MS.
    fn mapd(&mut self, slot: u32, req: u16, most: u16, base: u16, parked: bool, now: f64) {
        let max = self.o.map_max.min(if parked { MAPD_MAX_Z } else { MAPD_MAX }).min(most as usize);
        let Some(p) = self.player(slot) else { return };
        if p.impostor || self.map_at.get(&p.vip).is_some_and(|&t| now < t) {
            return;
        }
        let (room, at, vip, port, hub_port, lobbies) = (p.room, p.p.unwrap_or_default(), p.vip, p.port, p.hub_port, p.lobbies);
        let (at_t, cands) = self.map_cands.remove(&room).unwrap_or_else(|| (now, self.map_room(room, now)));
        let Some(p) = self.player(slot) else { return };
        let mut links: Vec<u32> = p.links.iter().map(|l| l.slot).collect();
        links.sort_unstable();
        let mut list: Vec<MapCand> = cands
            .iter()
            .filter(|c| {
                c.slot != slot
                    && links.binary_search(&c.slot).is_err()
                    && !self.player(c.slot).is_some_and(|o| self.apart_from(slot, c.slot, p, o))
            })
            .copied()
            .collect();
        self.map_cands.insert(room, (at_t, cands));
        let total = list.len().min(u16::MAX as usize) as u16;
        if list.len() > max {
            list.select_nth_unstable_by_key(max, |c| order_key(distance(&at, &c.p)));
            list.truncate(max);
            list.sort_unstable_by_key(|c| c.slot);
        }
        let mut m = self.maps.remove(&vip).unwrap_or_default();
        if base != 0 && m.sent.as_ref().is_some_and(|s| s.0 == base) {
            m.held = m.sent.take();
        }
        let from: &[MapSent] = match &m.held {
            Some((r, l)) if base != 0 && *r == base => l,
            _ => &[],
        };
        let (mut b, (mut e, mut ends, mut next)) =
            (if from.is_empty() { 0 } else { base }, map_diff(from, &list, at_t, MAP_TOL_M, MAP_TOL_H, parked));
        let mut parts = map_parts(&ends);
        if parts.len() > wire::MAPD_PARTS {
            (b, (e, ends, next)) = (0, map_diff(&[], &list, at_t, MAP_TOL_M, MAP_TOL_H, parked));
            parts = map_parts(&ends);
        }
        for (k, &(a, z)) in parts.iter().enumerate() {
            let (s0, s1) = (if a == 0 { 0 } else { ends[a - 1] }, if z == 0 { 0 } else { ends[z - 1] });
            let mut f = vec![0u8; F_DATA + wire::mapd_len(s1 - s0)];
            f[0] = KIND_UDP;
            f[4..8].copy_from_slice(&ROAM_HUB_VIP.to_be_bytes());
            f[8..12].copy_from_slice(&vip.to_be_bytes());
            f[12..14].copy_from_slice(&hub_port.to_be_bytes());
            f[14..16].copy_from_slice(&port.to_be_bytes());
            wire::write_mapd(
                &mut f[F_DATA..],
                wire::MSG_MAPD,
                self.id,
                self.seq,
                req,
                b,
                k as u8,
                parts.len() as u8,
                total,
                (z - a) as u8,
                if parked { wire::HF_MAPZ } else { 0 },
                &e[s0..s1],
            );
            self.seq = self.seq.wrapping_add(1);
            (self.send)(vip, Bytes::from(f));
        }
        m.sent = Some((req, next));
        m.at = now;
        self.maps.insert(vip, m);
        self.map_at.insert(vip, now + self.o.map_gap_ms);
        if lobbies && !self.lobbies_at.get(&vip).is_some_and(|&t| now < t) {
            self.lobbies_at.insert(vip, now + MAP_LOBBIES_GAP_MS);
            self.lobbies(slot, req, now);
        }
    }

    /// The minimap's answer to the player in `slot` (roam_core.h RC_MSG_NEARD): as the world map's (`mapd`), but of the
    /// players within `radius` m of it (NEAR_M at most, across the map as the minimap shows them), the nearest `most`
    /// (NEAR_MAX at most), each where it is now (its last SNAP moved on at its velocity, `ahead`, not as of the tick:
    /// a marker 2 m off is placed again), in one datagram (from nothing when what changed would not fit); one answer
    /// to a browser every near_gap_ms at most.  The players come from the grid of every room's (`NearGrid`), made at
    /// the first question after a tick: a browser's question reads the rows of cells around its player (each row's
    /// players one after another, the nearest cells first) for the nearest 2 `most` within NEAR_KEEP_M more than
    /// `radius`, its candidates for NEAR_CANDS_MS or until its player is NEAR_MOVE_M on; each question picks from
    /// those, each where it is now.  (A crowd of 1000 is 1000
    /// players to read: one in a HashMap of cells, each player looked up, every question, cost 31 us a question at
    /// 12,000 players, 4 a second for each of them more than the hub's thread has.)
    fn neard(&mut self, slot: u32, req: u16, most: u16, base: u16, radius: u16, now: f64) {
        let max = NEAR_MAX.min(most as usize);
        let r = (radius as f64).min(NEAR_M);
        let Some(p) = self.player(slot) else { return };
        if p.impostor || self.near_at.get(&p.vip).is_some_and(|&t| now < t) {
            return;
        }
        let (room, vip, port, hub_port) = (p.room, p.vip, p.port, p.hub_port);
        if self.near_grid.as_ref().is_none_or(|g| g.at > now) {
            self.near_gens += 1;
            self.near_grid = Some(self.near_grid_of(now, self.near_gens));
        }
        let mut m = self.nears.remove(&vip).unwrap_or_default();
        let mut found = std::mem::take(&mut self.near_found);
        found.clear();
        let mut picks = std::mem::take(&mut self.near_picks);
        picks.clear();
        // this question's stamp (`near_mark`)
        let mut mark = std::mem::take(&mut self.near_mark);
        mark.resize(mark.len().max(self.slots.len()), 0);
        self.near_stamp = self.near_stamp.wrapping_add(1);
        if self.near_stamp == 0 {
            mark.fill(0);
            self.near_stamp = 1;
        }
        let stamp = self.near_stamp;
        {
            let (Some(p), Some(grid)) = (self.player(slot), self.near_grid.as_ref()) else { return };
            if let Some(me) = p.p.is_some().then(|| ahead(p, now)) {
                let fresh = m.cands_of.is_some_and(|(at, s, rr, mm, ox, oy)| {
                    s == slot && rr == radius && mm == most && now - at < NEAR_CANDS_MS && (me[0] - ox).hypot(me[1] - oy) < NEAR_MOVE_M
                });
                if !fresh {
                    // the asker and its links: none of its candidates
                    mark[slot as usize] = stamp;
                    for l in &p.links {
                        if let Some(x) = mark.get_mut(l.slot as usize) {
                            *x = stamp;
                        }
                    }
                    // (the grid has each player where it was at its making, up to a tick ago: its rows are read farther
                    // out, and each player moved on from there)
                    let keep = r + NEAR_KEEP_M;
                    let reach = keep + NEAR_SLACK_M;
                    let (x0, x1, y0, y1) =
                        (near_cell(me[0] - reach), near_cell(me[0] + reach), near_cell(me[1] - reach), near_cell(me[1] + reach));
                    let (mx, my, k2, since) = (me[0] as f32, me[1] as f32, (keep * keep) as f32, ((now - grid.at) / 1000.0) as f32);
                    let blocks = !self.blocks.is_empty();
                    // the players of the cells cx0..=cx1 of row cy within `keep`, past itself, its links and the ones it
                    // keeps apart from
                    let row = |found: &mut Vec<(u32, u32)>, cy: i32, cx0: i32, cx1: i32| {
                        if cx0 > cx1 {
                            return;
                        }
                        // (the same run as a search of all the keys for (room, cy, cx0) to (room, cy, cx1) finds)
                        let ri = grid.rows.partition_point(|e| (e.0, e.1) < (room, cy));
                        let Some(&(rr, rc, rs)) = grid.rows.get(ri) else { return };
                        if (rr, rc) != (room, cy) {
                            return;
                        }
                        let (rs, re) = (rs as usize, grid.rows.get(ri + 1).map_or(grid.keys.len(), |e| e.2 as usize));
                        let run = &grid.keys[rs..re];
                        let a = rs + run.partition_point(|k| k.2 < cx0);
                        let z = rs + run.partition_point(|k| k.2 <= cx1);
                        for (k, it) in grid.items[a..z].iter().enumerate() {
                            let dt = since.min(it.left);
                            let (dx, dy) = (it.x + it.vx * dt - mx, it.y + it.vy * dt - my);
                            let d2 = dx * dx + dy * dy;
                            if d2 > k2 || mark.get(it.slot as usize) == Some(&stamp) {
                                continue;
                            }
                            if blocks && self.player(it.slot).is_some_and(|o| self.apart_from(slot, it.slot, p, o)) {
                                continue;
                            }
                            // (a distance's bits are in its order: it is not negative)
                            found.push((d2.to_bits(), (a + k) as u32));
                        }
                    };
                    // the cells within NEAR_FIRST cells of its own first: in a crowd its candidates are all there (they
                    // cover NEAR_FIRST cells less the slack all round it), and the rest of the reach is not read
                    let (cx, cy) = (near_cell(me[0]), near_cell(me[1]));
                    let (fx0, fx1, fy0, fy1) =
                        ((cx - NEAR_FIRST).max(x0), (cx + NEAR_FIRST).min(x1), (cy - NEAR_FIRST).max(y0), (cy + NEAR_FIRST).min(y1));
                    for y in fy0..=fy1 {
                        row(&mut found, y, fx0, fx1);
                    }
                    let inner = (NEAR_FIRST as f64 * NEAR_CELL_M - NEAR_SLACK_M).min(keep);
                    let inner2 = (inner * inner) as f32;
                    if found.iter().filter(|x| f32::from_bits(x.0) <= inner2).count() < 2 * max {
                        for y in y0..=y1 {
                            if (fy0..=fy1).contains(&y) {
                                row(&mut found, y, x0, fx0 - 1);
                                row(&mut found, y, fx1 + 1, x1);
                            } else {
                                row(&mut found, y, x0, x1);
                            }
                        }
                    }
                    if found.len() > 2 * max {
                        found.select_nth_unstable(2 * max);
                        found.truncate(2 * max);
                    }
                    m.cands.clear();
                    m.cands.extend(found.iter().map(|x| grid.items[x.1 as usize].slot));
                    m.cand_k.clear();
                    m.cand_k.extend(found.iter().map(|x| x.1));
                    m.cand_k.sort_unstable();
                    m.cand_nth = grid.nth;
                    m.cands_of = Some((now, slot, radius, most, me[0], me[1]));
                    found.clear();
                }
                // its candidates' places in this grid (by their slots: the grid's list holds the ones the maps show), in
                // their order, once a grid
                if m.cand_nth != grid.nth {
                    m.cand_k.clear();
                    m.cand_k.extend(m.cands.iter().filter_map(|&q| grid.pos.get(q as usize).copied().filter(|&k| k != u32::MAX)));
                    m.cand_k.sort_unstable();
                    m.cand_nth = grid.nth;
                }
                // each candidate where the grid has it now, the nearest `max` within the radius (by distance, then
                // slot: which ones those are does not hang on the order they are read in, a slot is there once)
                let (mx, my, r2, since) = (me[0] as f32, me[1] as f32, (r * r) as f32, ((now - grid.at) / 1000.0) as f32);
                for &k in &m.cand_k {
                    let it = &grid.items[k as usize];
                    let dt = since.min(it.left);
                    let (dx, dy) = (it.x + it.vx * dt - mx, it.y + it.vy * dt - my);
                    let d2 = dx * dx + dy * dy;
                    if d2 <= r2 {
                        picks.push((d2.to_bits(), it.slot, k));
                    }
                }
                if picks.len() > max {
                    picks.select_nth_unstable(max);
                }
            }
        }
        let total = picks.len().min(u16::MAX as usize) as u16;
        // each one placed where it is now, as its last SNAP has it (one gone from free roam since the grid: not), read
        // from the grid's `live` in the grid's order (it holds the same as the players: checked in a debug build), then
        // in the order of their slots
        let picked = picks.len().min(max);
        picks[..picked].sort_unstable_by_key(|x| x.2);
        let mut list = std::mem::take(&mut self.near_list);
        list.clear();
        if let Some(grid) = self.near_grid.as_ref() {
            for &(_, q, k) in &picks[..picked] {
                let s = grid.live.get(k as usize).copied().unwrap_or_default();
                debug_assert!(
                    Shown::same(&s, &self.player(q).map(Shown::of).unwrap_or_default()),
                    "the minimap grid's slot {q} out of date: {s:?}"
                );
                if s.live && s.room == room && self.shown_on_map(q, &s, now) {
                    list.push(map_cand_of(q, s.id, s.p, s.heading, s.v, ahead_of(s.p, s.v, s.pos_at, now), false));
                }
            }
        }
        list.sort_unstable_by_key(|c| c.slot);
        self.near_picks = picks;
        self.near_found = found;
        self.near_mark = mark;
        if base != 0 && m.sent.as_ref().is_some_and(|s| s.0 == base) {
            m.held = m.sent.take();
        }
        let from: &[MapSent] = match &m.held {
            Some((q, l)) if base != 0 && *q == base => l,
            _ => &[],
        };
        let (mut b, (mut e, mut ends, mut next)) =
            (if from.is_empty() { 0 } else { base }, map_diff(from, &list, now, NEAR_TOL_M, NEAR_TOL_H, false));
        if e.len() > wire::MAPD_BYTES || ends.len() > 255 {
            (b, (e, ends, next)) = (0, map_diff(&[], &list, now, NEAR_TOL_M, NEAR_TOL_H, false));
        }
        self.near_list = list;
        let mut f = vec![0u8; F_DATA + wire::mapd_len(e.len())];
        f[0] = KIND_UDP;
        f[4..8].copy_from_slice(&ROAM_HUB_VIP.to_be_bytes());
        f[8..12].copy_from_slice(&vip.to_be_bytes());
        f[12..14].copy_from_slice(&hub_port.to_be_bytes());
        f[14..16].copy_from_slice(&port.to_be_bytes());
        wire::write_mapd(&mut f[F_DATA..], wire::MSG_NEARD, self.id, self.seq, req, b, 0, 1, total, ends.len() as u8, 0, &e);
        self.seq = self.seq.wrapping_add(1);
        (self.send)(vip, Bytes::from(f));
        self.st.nears += 1;
        m.sent = Some((req, next));
        m.at = now;
        self.nears.insert(vip, m);
        // (the links come first: a hub's thread busy with them answers each minimap a second or so, its game asks
        // again a second after a question not answered)
        let gap = if self.thread_busy >= NEAR_BUSY_HIGH {
            1000.0
        } else if self.thread_busy >= NEAR_BUSY_LOW {
            500.0
        } else {
            self.o.near_gap_ms
        };
        self.near_at.insert(vip, now + gap);
    }

    /// every room's players a map shows (`map_shown`) now, by room and NEAR_CELL_M cell of where each is (`ahead`): the
    /// hub's grid number `nth`
    fn near_grid_of(&self, now: f64, nth: u64) -> NearGrid {
        let mut v: Vec<((u32, i32, i32), NearItem)> = Vec::with_capacity(self.order.len());
        for &q in &self.order {
            let Some(o) = self.player(q).filter(|o| self.map_shown(q, o, now)) else { continue };
            let at = ahead(o, now);
            let left = (MAP_AHEAD_MS - (now - o.pos_at).max(0.0)).max(0.0) / 1000.0;
            let it = NearItem { slot: q, x: at[0] as f32, y: at[1] as f32, vx: o.v[0] as f32, vy: o.v[1] as f32, left: left as f32 };
            v.push(((o.room, near_cell(at[1]), near_cell(at[0])), it));
        }
        v.sort_unstable_by_key(|e| e.0);
        let mut pos = vec![u32::MAX; v.iter().map(|e| e.1.slot as usize + 1).max().unwrap_or(0)];
        let mut rows: Vec<(u32, i32, u32)> = Vec::new();
        for (k, e) in v.iter().enumerate() {
            pos[e.1.slot as usize] = k as u32;
            let (room, cy, _) = e.0;
            if rows.last().is_none_or(|r| (r.0, r.1) != (room, cy)) {
                rows.push((room, cy, k as u32));
            }
        }
        let live = v.iter().map(|e| self.player(e.1.slot).map(Shown::of).unwrap_or_default()).collect();
        NearGrid { at: now, nth, keys: v.iter().map(|e| e.0).collect(), rows, live, items: v.into_iter().map(|e| e.1).collect(), pos }
    }

    /// a player in `slot` the maps show: in free roam (or paused there), a position younger than map_fresh_ms, no
    /// impostor, a slot a MAPD key can name
    fn map_shown(&self, slot: u32, o: &Player, now: f64) -> bool {
        slot < wire::MAPD_GONE as u32 && !o.impostor && o.p.is_some() && now - o.pos_at <= self.o.map_fresh_ms && matches!(o.status, 2 | 5)
    }

    /// `map_shown` of the player in `slot`, as `shown` has it
    fn shown_on_map(&self, slot: u32, s: &Shown, now: f64) -> bool {
        slot < wire::MAPD_GONE as u32 && !s.impostor && s.has_p && now - s.pos_at <= self.o.map_fresh_ms && matches!(s.status, 2 | 5)
    }

    /// the players of `room` a world map shows (MAPD), by slot: in free roam (or paused there), a position younger
    /// than map_fresh_ms, no impostor; each where it is now, moved on from its last SNAP at its velocity (MAP_AHEAD_MS
    /// at most).  A player sends the hub its SNAP twice a second: a position as it came would be up to half a second
    /// behind where the page has the marker, a car going straight would look off by that much, and each new SNAP
    /// would put its marker back or forward.
    fn map_room(&self, room: u32, now: f64) -> Vec<MapCand> {
        let mut list: Vec<MapCand> = self
            .order
            .iter()
            .filter_map(|&q| {
                let o = self.player(q)?;
                (o.room == room && self.map_shown(q, o, now)).then(|| map_cand(q, o, ahead(o, now), now))
            })
            .collect();
        list.sort_unstable_by_key(|c| c.slot);
        list
    }

    /// With the world map's answer, to a player that takes race lobbies: the open lobbies of its room (any player's,
    /// its own too, an advert younger than LOBBY_STALE_MS), nearest first, LOBBIES_MAX at most, in one datagram
    fn lobbies(&mut self, slot: u32, req: u16, now: f64) {
        let Some(p) = self.player(slot) else { return };
        if !p.lobbies || p.impostor {
            return;
        }
        let (room, at) = (p.room, p.p.unwrap_or_default());
        let mut near: Vec<(u64, (i16, i16, u32, u8, u8, u8))> = Vec::new();
        for &q in &self.order {
            let Some(o) = self.player(q) else { continue };
            let Some((l, t)) = o.lobby else { continue };
            if o.impostor || o.room != room || now - t > LOBBY_STALE_MS {
                continue;
            }
            if self.apart_from(slot, q, p, o) {
                continue;
            }
            let d = distance(&at, &[l.x as f64, l.y as f64, at[2]]);
            near.push((order_key(d), (l.x, l.y, l.race, l.n, l.max, l.state)));
        }
        near.sort_by_key(|e| e.0);
        near.truncate(wire::LOBBIES_MAX);
        let list: Vec<(i16, i16, u32, u8, u8, u8)> = near.into_iter().map(|e| e.1).collect();
        let (vip, port, hub_port) = (p.vip, p.port, p.hub_port);
        let mut f = vec![0u8; F_DATA + wire::lobbies_len(list.len())];
        f[0] = KIND_UDP;
        f[4..8].copy_from_slice(&ROAM_HUB_VIP.to_be_bytes());
        f[8..12].copy_from_slice(&vip.to_be_bytes());
        f[12..14].copy_from_slice(&hub_port.to_be_bytes());
        f[14..16].copy_from_slice(&port.to_be_bytes());
        wire::write_lobbies(&mut f[F_DATA..], self.id, self.seq, req, &list);
        self.seq = self.seq.wrapping_add(1);
        (self.send)(vip, Bytes::from(f));
    }

    /// browser `vip` left the switch
    pub fn peer_gone(&mut self, vip: u32) {
        self.blocks.remove(&vip);
        self.pages.remove(&vip);
        self.friends.remove(&vip);
        self.room_names.remove(&vip);
        if !self.per_vip.contains_key(&vip) {
            return;
        }
        let mine: Vec<u32> = self.order.iter().copied().filter(|&s| self.player(s).is_some_and(|p| p.vip == vip)).collect();
        self.drop_slots(&mine, true);
    }

    /// browser `vip`'s page blocks these players, by the name over their car (`Block`): logged, so that the operator
    /// sees who many block
    pub fn blocks(&mut self, vip: u32, names: Vec<String>) {
        let list: Vec<String> = names.iter().map(|n| format!("{n:?}")).collect();
        (self.log)(&format!("[roam] {} blocks {}", vip_str(vip), list.join(", ")));
        let mut b = Block::new(&names);
        b.owners_of(self.claims.as_deref(), (self.now)());
        self.blocks.insert(vip, b);
    }

    /// Browser `vip`'s keyphrase (`me`) has these friends (their keyphrases: the server reads its friends list when its
    /// page asks): each one in its room is linked with it first, while each one's list here names the other
    /// (`friend_id`).  A friendship goes both ways in the server's lists, so each of these friends in free roam now (as
    /// of the last tick) has `me` on its list here too: one befriended while it was away is linked from now on.
    pub fn friends(&mut self, vip: u32, me: Arc<str>, owners: Vec<Arc<str>>) {
        let theirs: Vec<u32> =
            owners.iter().filter_map(|o| self.player(*self.owners_at.get(o)?)).map(|p| p.vip).filter(|&v| v != vip).collect();
        if owners.is_empty() {
            self.friends.remove(&vip);
        } else {
            self.friends.insert(vip, owners);
        }
        for v in &theirs {
            let l = self.friends.entry(*v).or_default();
            if !l.contains(&me) && l.len() < FRIENDS_MAX {
                l.push(me.clone());
            }
        }
        for v in std::iter::once(vip).chain(theirs) {
            let f = self.friends.get(&v);
            for p in self.slots.iter_mut().flatten().filter(|p| p.vip == v) {
                p.friend_ids = friend_ids(p.owner.as_ref(), f);
            }
        }
    }

    /// browser `vip`'s page plays in the room of this name (the hub has only its hash otherwise): told to its friends,
    /// for their Join (`whereabouts`)
    pub fn room_name(&mut self, vip: u32, name: String) {
        self.room_names.insert(vip, name.chars().take(64).collect());
    }

    /// The player of browser `vip` is linked now with one under `name` (its Online form, as nfsu2-claims compares
    /// names), its car within FRIEND_NEAR_M of this one's (their last SNAPs): that player's browser and keyphrase
    /// (None: its page plays with none; a name under another keyphrase's claim has no links).  A page may ask to
    /// befriend only a player whose car touched its own, never just any name (a room of fewer players than the links
    /// a player has links them all, near or not).
    pub fn linked_with(&self, vip: u32, name: &str) -> Option<(u32, Option<Arc<str>>)> {
        let canon = nfsu2_claims::canonical(name)?;
        self.slots.iter().flatten().filter(|p| p.vip == vip && !p.impostor).find_map(|p| {
            p.links.iter().find_map(|l| {
                let q = self.player(l.slot)?;
                let near = matches!((p.p, q.p), (Some(a), Some(b)) if {
                    let (dx, dy, dz) = (a[0] - b[0], a[1] - b[1], a[2] - b[2]);
                    dx * dx + dy * dy + dz * dz <= FRIEND_NEAR_M * FRIEND_NEAR_M
                });
                (q.canon == canon && !q.impostor && q.vip != vip && near).then(|| (q.vip, q.owner.clone()))
            })
        })
    }

    /// Where each of these keyphrases plays in free roam (as of the last tick), None: it does not
    pub fn whereabouts(&self, owners: &[Arc<str>]) -> Vec<Option<Whereabouts>> {
        owners
            .iter()
            .map(|o| {
                let p = self.player(*self.owners_at.get(o)?)?;
                Some(Whereabouts { room: p.room, room_name: self.room_names.get(&p.vip).cloned() })
            })
            .collect()
    }

    /// browser `vip`'s page goes by `name` on the switch (its hello's): its players' names are held against it
    /// (`name_check`)
    pub fn page_name(&mut self, vip: u32, name: String) {
        self.pages.insert(vip, Page { name, told: Vec::new(), flew: false });
        if self.per_vip.contains_key(&vip) {
            let mine: Vec<u32> = self.order.iter().copied().filter(|&s| self.player(s).is_some_and(|p| p.vip == vip)).collect();
            for s in mine {
                self.name_check(s);
            }
        }
    }

    /// Browser `vip`'s game says its car was in the air in free roam for `secs` (runtime/roam_game.c fly_frame), and its
    /// page leaves the switch for the rest of its game, as after a kick: a `[cheat]` line, with where the car was last
    /// and how many other players of its room in free roam were within FLEW_SEEN_M of it.  Once a page: the page says
    /// so before it goes, and nothing else comes of it here.
    pub fn flew(&mut self, vip: u32, secs: f64) {
        let page = self.pages.get_mut(&vip);
        if page.as_ref().is_some_and(|p| p.flew) {
            return;
        }
        let page_name = page.map(|p| {
            p.flew = true;
            p.name.clone()
        });
        let me = self.players().filter(|p| p.vip == vip && p.p.is_some()).max_by(|a, b| a.pos_at.total_cmp(&b.pos_at));
        let (name, at) = match (me, page_name) {
            (Some(p), _) => (p.name.clone(), p.p.map(|a| (a, p.room))),
            (None, Some(n)) => (n, None),
            (None, None) => (String::new(), None),
        };
        let place = match at {
            Some((a, room)) => {
                let seen = self
                    .players()
                    .filter(|o| o.vip != vip && o.room == room && matches!(o.status, 2 | 5))
                    .filter(|o| o.p.is_some_and(|b| (b[0] - a[0]).hypot(b[1] - a[1]) < FLEW_SEEN_M))
                    .count();
                format!(
                    ", last at {:.0} {:.0} {:.0}, {seen} other player{} within {FLEW_SEEN_M:.0} m",
                    a[0],
                    a[1],
                    a[2],
                    if seen == 1 { "" } else { "s" }
                )
            }
            None => ", not on the hub now".into(),
        };
        self.st.cheat_lines += 1;
        (self.log)(&format!(
            "[cheat] {} \"{}\": its car was in the air in free roam for {:.0} s, by its own game{place}: its page left the switch",
            vip_str(vip),
            js::printable(&name, 32),
            if secs.is_finite() { secs.clamp(0.0, 86400.0) } else { 0.0 }
        ));
    }

    /// The name over the car of the player in `slot` (its HELLO's) against its page's on the switch: another one is a
    /// console line, so that the operator finds the player by the name the others see (its `who` finds both).  Each
    /// name once, NAME_LINES a page at most.  A page that is not known yet is held against it when it is.
    fn name_check(&mut self, slot: u32) {
        let Some(Some(p)) = self.slots.get(slot as usize) else { return };
        let Some(page) = self.pages.get_mut(&p.vip) else { return };
        if p.name.is_empty() || same_name(&page.name, &p.name) || page.told.len() >= NAME_LINES || page.told.contains(&p.name) {
            return;
        }
        page.told.push(p.name.clone());
        (self.log)(&format!(
            "[roam] {} \"{}\": its game shows \"{}\" over its car",
            vip_str(p.vip),
            js::printable(&page.name, 32),
            js::printable(&p.name, 32)
        ));
    }

    /// the names over the cars of the players in free roam that are not their pages' on the switch (all of them for a
    /// page not known), by browser, for the operator's `who`
    pub fn other_names(&self) -> HashMap<u32, Vec<String>> {
        let mut by: HashMap<u32, Vec<String>> = HashMap::new();
        for p in self.players().filter(|p| !p.name.is_empty()) {
            if self.pages.get(&p.vip).is_some_and(|g| same_name(&g.name, &p.name)) {
                continue;
            }
            let names = by.entry(p.vip).or_default();
            if !names.contains(&p.name) {
                names.push(p.name.clone());
            }
        }
        by
    }

    /// whether the players in slots a and b (p and q) are kept apart: at the last tick (`apart`, the race lobbies'
    /// pairs too), or by a block since
    fn apart_from(&self, a: u32, b: u32, p: &Player, q: &Player) -> bool {
        !self.blocks.is_empty() && (self.apart.binary_search(&(a.min(b), a.max(b))).is_ok() || blocked(&self.blocks, p, q))
    }

    /// every `tick_ms`: who is linked to whom, and every player's list
    pub fn tick(&mut self) {
        let t0 = Instant::now();
        let now = (self.now)();
        self.adjust(now);
        let gone_ms = self.o.gone_ms;
        let gone: Vec<u32> = self.order.iter().copied().filter(|&s| self.player(s).is_some_and(|p| now - p.seen > gone_ms)).collect();
        self.drop_slots(&gone, true);
        // every player gone since the last tick out of the links the tick weighs
        self.purge();
        self.map_at.retain(|_, t| now < *t);
        self.lobbies_at.retain(|_, t| now < *t);
        self.maps.retain(|_, m| now - m.at < MAP_KEEP_MS);
        self.map_cands.clear();
        self.near_at.retain(|_, t| now < *t);
        self.nears.retain(|_, m| now - m.at < MAP_KEEP_MS);
        self.near_grid = None;
        // the lobbies not heard of for JOINED_KEEP_MS, every minute
        if now - self.joined_pruned >= 60_000.0 {
            self.joined_pruned = now;
            self.joined.retain(|_, j| now - j.at < JOINED_KEEP_MS);
            let joined = &self.joined;
            self.joined_by_vip.retain(|_, ls| {
                ls.retain(|l| joined.contains_key(l));
                !ls.is_empty()
            });
        }
        // what the cheat watch did not tell, summed up now and then; and the watches of players gone a while, summed up
        // as of when they went
        let mut gone_for_good = Vec::new();
        self.kept.retain(|&k, w| {
            if now - w.at < WATCH_KEEP_MS {
                return true;
            }
            gone_for_good.extend(w.watch.summary((k >> 16) as u32, &w.name, w.at, true));
            false
        });
        for line in gone_for_good {
            self.st.cheat_lines += 1;
            (self.log)(&line);
        }
        // the races whose results all came (or a while ago): each one's result, and the ones won by cheating
        for (o, v) in self.results.judged(now) {
            let who = |id: u32| o.names.get(&id).cloned().or_else(|| self.who(id)).unwrap_or_else(|| format!("player {id:08x}"));
            let result = results::result_line(&o, &who);
            let cheat = v.map(|v| results::line(&v, &who));
            (self.log)(&result);
            if let Some(line) = cheat {
                self.st.cheat_lines += 1;
                (self.log)(&line);
            }
        }
        for &slot in &self.order {
            if let Some(Some(p)) = self.slots.get_mut(slot as usize)
                && let Some(line) = p.watch.summary(p.vip, &p.name, now, false)
            {
                self.st.cheat_lines += 1;
                (self.log)(&line);
            }
        }

        // the keyphrases that hold the names blocked, when the claims changed
        if let Some(c) = self.claims.as_deref() {
            self.blocks.values_mut().for_each(|b| b.owners_of(Some(c), now));
        }
        self.apart.clear();

        // the rooms: the players with a position younger than `stale_ms`, in the order they came (and by keyphrase,
        // where friends find them: `whereabouts`)
        let s = &mut self.s;
        s.room_of.clear();
        self.owners_at.clear();
        let mut rooms = 0;
        // (no player in an outrun, no outrun's pair to look for)
        let mut dueling = 0;
        for &slot in &self.order {
            let Some(Some(p)) = self.slots.get(slot as usize) else { continue };
            if p.p.is_none() || now - p.pos_at > self.o.stale_ms || p.impostor {
                continue;
            }
            dueling += (p.duel != 0) as usize;
            if let Some(o) = &p.owner {
                self.owners_at.insert(o.clone(), slot);
            }
            let r = *s.room_of.entry(p.room).or_insert_with(|| {
                if rooms == s.rooms.len() {
                    s.rooms.push(Vec::new());
                }
                s.rooms[rooms].clear();
                rooms += 1;
                rooms - 1
            });
            s.rooms[r].push(slot);
        }
        // (empty already, unless a tick before broke off)
        s.fresh.iter_mut().for_each(Vec::clear);
        s.fresh.resize_with(self.slots.len(), Vec::new);
        s.local.clear();
        s.local.resize(self.slots.len(), NONE);
        let level = Level::new(self.o.level_slack_m, self.o.level_grade, self.o.level_k);
        let w = Weigh { now, hold_ms: self.o.hold_ms, keep: self.o.keep, threads: self.o.link_threads, level };
        let (mut duel_links, mut lobby_links, mut crew_links, mut crew_skips) = (0, 0, 0, 0);
        for r in 0..rooms {
            let list = std::mem::take(&mut s.rooms[r]);
            prepare(&self.slots, &list, self.cap, &self.o, now, s);
            if dueling > 1 {
                duel_pairs(&self.slots, &list, s);
            } else {
                s.dpairs.clear();
            }
            lobby_pairs(&self.slots, &list, now, s);
            block_pairs(&self.slots, &list, &self.blocks, s);
            self.apart.extend(s.bpairs.iter().map(|&(a, b)| (list[a as usize], list[b as usize])).map(|(x, y)| (x.min(y), x.max(y))));
            // (no player in a crew and none with friends, nothing to look for)
            if self.crewed > 0 || !self.friends.is_empty() {
                crew_skips += crew_pairs(&self.slots, &list, &w, s);
            } else {
                s.cpairs.clear();
            }
            let (duel, lobby, crew) = if list.len() > self.o.exact_room {
                link_many(&self.slots, &list, &w, s)
            } else {
                link_every_pair(&self.slots, &list, &w, s)
            };
            duel_links += duel;
            lobby_links += lobby;
            crew_links += crew;
            for &slot in &list {
                s.local[slot as usize] = NONE;
            }
            s.rooms[r] = list;
        }
        self.apart.sort_unstable();
        self.st.duel_links = duel_links;
        self.st.lobby_links = lobby_links;
        self.st.crew_links = crew_links;
        self.st.crew_skips = crew_skips;
        self.lists();
        let ms = t0.elapsed().as_secs_f64() * 1000.0;
        self.st.ticks += 1;
        if ms > self.st.tick_ms_max {
            self.st.tick_ms_max = ms;
        }
    }

    /// every player's new links in place, and its PEERS list sent.  The lists are cut from blocks of LIST_BLOCK
    /// bytes, about 11 lists each: the switch queues a list as it is until the player's writer takes it, and one
    /// buffer for all of them (700 KB for 2000 players) was kept whole for as long as any list of its tick waited in
    /// a slow player's queue, one such buffer a second; a list of its own each was 2000 allocations a tick.
    fn lists(&mut self) {
        let s = &mut self.s;
        for &slot in &self.order {
            let Some(Some(p)) = self.slots.get(slot as usize) else { continue };
            let links = &s.fresh[slot as usize];
            let slots = &self.slots;
            // its crews and, a link each, which of them that player is in too: only to a player in a crew (so no crews
            // but its own go out); and which are its friends (both ways: the friendship's id is both players'), to a
            // player with friends whose core reads it
            let mine = if self.crewed > 0 { &p.crews[..crews_of(p).count()] } else { &[][..] };
            let befriended = p.marks && !p.friend_ids.is_empty();
            let with = !mine.is_empty() || befriended;
            s.marks.clear();
            if with {
                s.marks.extend(links.iter().map(|l| match slots.get(l.slot as usize) {
                    Some(Some(q)) => {
                        let crews = mine.iter().enumerate().fold(0u8, |m, (c, h)| m | (q.crews.contains(h) as u8) << c);
                        let friend = befriended
                            && match (&p.owner, &q.owner) {
                                (Some(a), Some(b)) if a != b => {
                                    let id = friend_id(a, b);
                                    p.friend_ids.binary_search(&id).is_ok() && q.friend_ids.binary_search(&id).is_ok()
                                }
                                _ => false,
                            };
                        crews | if friend { wire::MARK_FRIEND } else { 0 }
                    }
                    _ => 0,
                }));
            }
            let n = F_DATA + if with { wire::peers_crew_len(links.len(), mine.len()) } else { wire::peers_len(links.len()) };
            if s.buf.capacity() < n {
                s.buf = BytesMut::with_capacity(LIST_BLOCK.max(n));
            }
            let buf = &mut s.buf;
            buf.resize(n, 0);
            buf[0] = KIND_UDP;
            buf[4..8].copy_from_slice(&ROAM_HUB_VIP.to_be_bytes());
            buf[8..12].copy_from_slice(&p.vip.to_be_bytes());
            buf[12..14].copy_from_slice(&p.hub_port.to_be_bytes());
            buf[14..16].copy_from_slice(&p.port.to_be_bytes());
            let list = links.iter().map(|l| match slots.get(l.slot as usize) {
                Some(Some(q)) => PeerEntry { vip: q.vip, port: q.port, id: q.id },
                _ => PeerEntry { vip: 0, port: 0, id: 0 },
            });
            wire::write_peers(&mut buf[F_DATA..], self.id, self.seq, list, mine, &s.marks);
            self.seq = self.seq.wrapping_add(1);
            if (self.send)(p.vip, buf.split().freeze()) {
                self.st.lists += 1;
            }
        }
        for &slot in &self.order {
            if let Some(Some(p)) = self.slots.get_mut(slot as usize) {
                std::mem::swap(&mut p.links, &mut s.fresh[slot as usize]);
                s.fresh[slot as usize].clear();
            }
        }
    }

    /// the cap on the links: a quarter down while the server is busy, 2 up once it has room again
    fn adjust(&mut self, now: f64) {
        let busy = (self.busy)();
        let old = self.cap;
        // a cut comes down from the most links any player has (32 for every core before version 10, while the hub's
        // most is 48): from the hub's most, the first cut would have cut nobody's
        let top = self.players().map(|p| p.degree).max().unwrap_or(self.o.degree);
        let mut was = old;
        self.st.busy = js_round(busy * 100.0);
        let o = &self.o;
        if busy > o.busy_high && now - self.cap_at >= o.cap_down_ms {
            was = old.min(top.max(o.min_degree));
            self.cap = o.min_degree.min(o.degree).max(was * 3 / 4);
        } else if busy < o.busy_low && now - self.cap_at >= o.cap_up_ms {
            self.cap = o.degree.min(self.cap + 2);
        }
        if self.cap == old {
            return;
        }
        self.cap_at = now;
        if self.cap < was {
            self.st.cap_cuts += 1;
        }
        (self.log)(&format!(
            "[roam] the server's thread was {} % busy: at most {} links a player (was {was})",
            js::number_to_string(self.st.busy),
            self.cap
        ));
    }

    /// how busy the hub's own thread was over the last second (its ticks and frames), 0 to 1: the minimap's answers come
    /// less often while it is busy (NEAR_BUSY_LOW, NEAR_BUSY_HIGH)
    pub fn set_thread_busy(&mut self, busy: f64) {
        self.thread_busy = if busy.is_finite() { busy.clamp(0.0, 1.0) } else { 0.0 };
    }

    /// free roam is busy: the cap is under `ask_cap` (the start page does not ask new players then)
    pub fn crowded(&self) -> bool {
        self.cap < self.o.ask_cap
    }

    /// the browsers in free roam now (a HELLO within `gone_ms`)
    pub fn vips(&self) -> HashSet<u32> {
        self.players().map(|p| p.vip).collect()
    }

    /// how many browsers drive in free roam now, or paused there, as their last HELLO or SNAP says (the server's
    /// /players `roam`): one a browser whatever its endpoints; in a race, a shop, the menus or loading, not
    pub fn in_roam(&self) -> usize {
        self.players().filter(|p| matches!(p.status, 2 | 5)).map(|p| p.vip).collect::<HashSet<u32>>().len()
    }

    /// `in_roam()` by room, for the rooms with a name (room 0, none, is everyone's and not here): the server's
    /// /players `room`, for the start page of a player who set one
    pub fn in_rooms(&self) -> HashMap<u32, usize> {
        let mut seen = HashSet::new();
        let mut n = HashMap::new();
        for p in self.players().filter(|p| p.room != 0 && matches!(p.status, 2 | 5) && !p.impostor) {
            if seen.insert((p.room, p.vip)) {
                *n.entry(p.room).or_insert(0) += 1;
            }
        }
        n
    }

    /// `in_roam()` by crew, in any room: (crew, browsers) for each crew that a player in free roam, or paused there,
    /// names, in the order of the crews' numbers (each browser once a crew, whatever its endpoints), into `out`, which
    /// is kept from tick to tick (`RoamHub` keeps this after each tick for the server's /players `crews`)
    pub fn crew_counts(&self, out: &mut Vec<(u32, u32)>) {
        out.clear();
        if self.crewed == 0 {
            return;
        }
        for p in self.players().filter(|p| matches!(p.status, 2 | 5) && !p.impostor) {
            out.extend(crews_of(p).map(|c| (c, p.vip)));
        }
        // each (crew, browser) once, then each crew's run counted, in place
        out.sort_unstable();
        out.dedup();
        out.iter_mut().for_each(|e| e.1 = 1);
        out.dedup_by(|e, kept| {
            let same = e.0 == kept.0;
            kept.1 += same as u32;
            same
        });
    }

    /// `in_roam()` in each of these crews (any room), in their order: the server's /players `crews`, for the start
    /// page of a player who set crews
    pub fn in_crews(&self, crews: &[u32]) -> Vec<usize> {
        let mut counts = Vec::new();
        self.crew_counts(&mut counts);
        crews.iter().map(|&c| crew_count(&counts, c)).collect()
    }

    /// for the tests and the switch's `info()` (the server's console): counts only, never a position
    pub fn info(&self) -> Value {
        let (mut linked, mut links) = (0u64, 0u64);
        for p in self.players() {
            let n = if self.dead.is_empty() { p.links.len() } else { p.links.iter().filter(|l| self.live(l)).count() };
            if n > 0 {
                linked += 1;
            }
            links += n as u64;
        }
        let st = &self.st;
        json!({
            "players": self.by_key.len(),
            "linked": linked,
            "links": num(links as f64 / 2.0),
            "cap": self.cap,
            "frames": st.frames,
            "bad": st.bad,
            "refused": st.refused,
            "lists": st.lists,
            "ticks": st.ticks,
            "tickMsMax": num(st.tick_ms_max),
            "busy": num(st.busy),
            "capCuts": st.cap_cuts,
            "duelLinks": st.duel_links,
            "lobbyLinks": st.lobby_links,
            "crewLinks": st.crew_links,
            "crewSkips": st.crew_skips,
            "raceReports": st.race_reports,
            "cheatLines": st.cheat_lines,
            "nears": st.nears,
            "threadBusy": num(self.thread_busy),
            "watchesKept": self.kept.len(),
            "blocking": self.blocks.len(),
            "keptApart": self.apart.len(),
        })
    }

    /// each pair of browsers linked now, once (the lower address first; a browser's endpoints are one), sorted: the
    /// server's counts of the players' paths (nfsu2-lan paths.rs).  Addresses only, never a position.
    pub fn link_pairs(&self) -> Vec<(u32, u32)> {
        let mut v = Vec::new();
        for p in self.players() {
            let to = p.links.iter().filter(|l| self.live(l)).filter_map(|l| self.player(l.slot)).map(|o| o.vip);
            v.extend(to.filter(|&o| o != p.vip).map(|o| (p.vip.min(o), p.vip.max(o))));
        }
        v.sort_unstable();
        v.dedup();
        v
    }

    /// each player's links now, by `vip:port`, for the tests
    pub fn links(&self) -> Vec<(String, Vec<String>)> {
        let name = |p: &Player| format!("{}:{}", p.vip, p.port);
        let to = |l: &Link| self.live(l).then(|| self.player(l.slot)).flatten();
        self.players().map(|p| (name(p), p.links.iter().filter_map(to).map(name).collect())).collect()
    }
}

/// a SNAP comes every half second: a position is moved on by its age up to this (s), a game stalled no farther
const AGE_MAX_S: f64 = 1.0;

/// A room's players by their place in its list: positions, degrees (under the cap), and each one's place.  Where the
/// links weigh a player: where it will be soon_ms and look_ms on at its speed, from where its last SNAP had it (moved
/// on by that SNAP's age), and its view point, view_m ahead of where its car points from its place soon_ms on (the
/// camera looks that way: what is in front shows from farther away).  Only a car in free roam moves on and has a view
/// point (a paused game's last SNAP has the speed it had), and a player not in free roam (a shop, a race, the menus,
/// loading) is away_m up, out of the way of the cars shown.
///
/// The heights say which road a car is on (`Level`), so they go with the road: the place soon is at the height its
/// SNAP had (a car's speed up or down is a jump's as often as a hill's: moved on, one leaving a ramp at 8 m/s was 8 m
/// up a second on, over a road above), and the place ahead and the view point, a hundred metres on and more, are up
/// or down the slope its car's nose is on (wire `slope`) for the way they are along it: else a car 150 m on up a hill
/// was 20 m over the view point, and weighed as on a road above.  The "up" of a player not in free roam is a place's
/// fourth number, apart from its height.
fn prepare(slots: &[Option<Player>], list: &[u32], cap: usize, o: &RoamOptions, now: f64, s: &mut Scratch) {
    s.pos.clear();
    s.soon.clear();
    s.ahead.clear();
    s.view.clear();
    s.deg.clear();
    s.ccap.clear();
    for (i, &slot) in list.iter().enumerate() {
        let p = player(slots, slot);
        let (at, v, status, heading, slope, age) = p.map_or(([0.0; 3], [0.0; 3], 0, 0, 0, 0.0), |p| {
            (p.p.unwrap_or_default(), p.v, p.status, p.heading, p.slope, (now - p.pos_at) / 1000.0)
        });
        let (v, view) = if status == 2 { (v, o.view_m) } else { ([0.0; 3], 0.0) };
        let up = if matches!(status, 2 | 5) { 0.0 } else { o.away_m };
        let age = age.clamp(0.0, AGE_MAX_S);
        let a = f64::from(heading) * std::f64::consts::PI / 128.0;
        let (fx, fy, s1) = (a.cos(), a.sin(), f64::from(slope) / 100.0);
        // t seconds on, and how far that is along where its nose points (backwards: down a slope it points up)
        let along = v[0] * fx + v[1] * fy;
        let on = |t: f64| [at[0] + v[0] * t, at[1] + v[1] * t, at[2] + s1 * along * t, up];
        let ts = age + o.soon_ms / 1000.0;
        let soon = [at[0] + v[0] * ts, at[1] + v[1] * ts, at[2], up];
        s.pos.push(at);
        s.soon.push(soon);
        s.ahead.push(on(age + o.look_ms / 1000.0));
        s.view.push([soon[0] + fx * view, soon[1] + fy * view, at[2] + s1 * (along * ts + view), up]);
        // a player's links at most: its degree, under the cap
        s.deg.push(p.map_or(0, |p| p.degree.min(cap)));
        s.ccap.push(p.map_or(0, |p| crew_cap(p.degree)));
        s.local[slot as usize] = i as u32;
    }
}

fn player(slots: &[Option<Player>], slot: u32) -> Option<&Player> {
    slots.get(slot as usize)?.as_ref()
}

/// the place in the room of the player in `slot` (None: not in this room)
fn local(s: &[u32], slot: u32) -> Option<usize> {
    s.get(slot as usize).copied().filter(|&j| j != NONE).map(|j| j as usize)
}

/// a link made: both ends list each other
fn join(fresh: &mut [Vec<Link>], a: u32, b: u32, since: f64) {
    fresh[a as usize].push(Link { slot: b, since });
    fresh[b as usize].push(Link { slot: a, since });
}

/// since when the players in slots a and b are linked: since the link of the last tick, or now
fn since_of(slots: &[Option<Player>], a: u32, b: u32, now: f64) -> f64 {
    player(slots, a).and_then(|p| p.links.iter().find(|l| l.slot == b)).map_or(now, |l| l.since)
}

/// The links outruns between players need before any other (roam_game.c du_*: the game races the other player's car
/// as its own peer shows it): two players of the room who each name the other as their outrun's (`Player::duel`, its
/// HELLO's since core version 11), into `s.dpairs` as places in the room, the lower first.  By distance alone the one
/// who fell behind was let go as soon as a player's links' worth of others were nearer (40 a computer, 16 a phone, 8
/// under the busy cap: a crowd round the start, the 62 m of a phone's log of 2026-11-01), and both games saw the other
/// leave and won.  Only the two together: a HELLO names whom it likes, and one player's word alone would have the hub
/// link it first with anyone of its room.  Looks only at the players who name someone (a sorted list of them, no map).
fn duel_pairs(slots: &[Option<Player>], list: &[u32], s: &mut Scratch) {
    let Scratch { dpairs, dids, .. } = s;
    dpairs.clear();
    dids.clear();
    dids.extend(list.iter().enumerate().filter_map(|(j, &q)| player(slots, q).filter(|p| p.duel != 0).map(|p| (p.id, j as u32))));
    if dids.len() < 2 {
        return;
    }
    dids.sort_unstable();
    for &(id, i) in dids.iter() {
        let Some(want) = player(slots, list[i as usize]).map(|p| p.duel) else { continue };
        // the one it names, naming it back (each pair once: from its lower place)
        for &(_, j) in dids[dids.partition_point(|e| e.0 < want)..].iter().take_while(|e| e.0 == want) {
            if i < j && player(slots, list[j as usize]).is_some_and(|q| q.duel == id) {
                dpairs.push((i, j));
            }
        }
    }
}

/// The links a race lobby needs before any other (roam_game.c mp_*: its messages go between linked players only): its
/// host with each of its players, with each player asking to join it (LOBBY_JOIN_MS), and with the LOBBY_NEAR_MAX
/// nearest players in free roam within LOBBY_NEAR_M of its start.  Without them a host at a busy start (the players around it, and the ones who went into the career race
/// there, whose places count for `stale_ms`) had its links full of nearer players than one who stopped there to join:
/// that one's link came and went, it heard of the lobby now and then, and its Enter went nowhere.  Into `s.lpairs`, as
/// places in the room, the host first; the link functions make them first, while both ends have room.
///
/// Only for a lobby whose start is where its host is, and for the players it lists who are there too (LOBBY_STAY_M):
/// an advert says what it likes, and one placed at a crowd, or listing the ids of players anywhere in the room, had
/// the hub link its sender with all of them first (the third security audit, 2026-10-23).
fn lobby_pairs(slots: &[Option<Player>], list: &[u32], now: f64, s: &mut Scratch) {
    let Scratch { pos, lpairs, lids, ljoin, lnear, lmembers, .. } = s;
    lpairs.clear();
    lids.clear();
    ljoin.clear();
    lmembers.clear();
    for (j, &q) in list.iter().enumerate() {
        if let Some((id, _)) = player(slots, q).and_then(|o| o.join).filter(|&(_, t)| now - t <= LOBBY_JOIN_MS) {
            ljoin.push((id, j as u32));
        }
    }
    for (i, &slot) in list.iter().enumerate() {
        let Some((l, t)) = player(slots, slot).and_then(|p| p.lobby) else { continue };
        // open or starting (RC_LS_*), its advert fresh, its start where its host is
        let (x, y) = (f64::from(l.x), f64::from(l.y));
        let there = |j: usize| (pos[j][0] - x).powi(2) + (pos[j][1] - y).powi(2) <= LOBBY_STAY_M * LOBBY_STAY_M;
        if now - t > LOBBY_STALE_MS || l.state > 1 || !there(i) {
            continue;
        }
        if lids.is_empty() {
            for (j, &q) in list.iter().enumerate() {
                if let Some(o) = player(slots, q) {
                    lids.insert(o.id, j as u32);
                }
            }
        }
        for id in &l.ids[1..(l.n as usize).clamp(1, wire::LOBBY_PLAYERS)] {
            if let Some(&j) = lids.get(id).filter(|&&j| j != i as u32 && there(j as usize)) {
                lpairs.push((i as u32, j));
                lmembers.push((i as u32, j));
            }
        }
        lpairs.extend(ljoin.iter().filter(|&&(id, j)| id == l.id && j != i as u32).map(|&(_, j)| (i as u32, j)));
        lnear.clear();
        for (j, &q) in list.iter().enumerate() {
            // in free roam or paused there (RC_ST_ROAM, RC_ST_PAUSED), as the map's
            if j == i || !player(slots, q).is_some_and(|o| matches!(o.status, 2 | 5)) {
                continue;
            }
            let d = (pos[j][0] - x).powi(2) + (pos[j][1] - y).powi(2);
            if d <= LOBBY_NEAR_M * LOBBY_NEAR_M {
                lnear.push((d, j as u32));
            }
        }
        lnear.sort_unstable_by(|a, b| a.0.total_cmp(&b.0).then(a.1.cmp(&b.1)));
        lpairs.extend(lnear.iter().take(LOBBY_NEAR_MAX).map(|&(_, j)| (i as u32, j)));
    }
}

/// The pairs of a room kept apart (`Hub::blocks`), into `s.bpairs` as places in the room, the lower first, sorted,
/// and each place's others (`s.bstart`, `s.bnext`; empty: no pair in the room): a player and one it blocks or that
/// blocks it, and a race lobby's host and a player kept apart from one of the lobby's players (not in it itself).  The
/// link to its host is how a player hears of a lobby and joins it: through another's lobby the two would race each
/// other.  The link functions make no link of a pair, and the world map and the lobby list leave each out for the
/// other (`Hub::apart`).  Two who join one lobby within a tick of each other are both in it.  Only a room with a
/// blocker in it looks: at each of its players once, then up each blocker's names and keyphrases.
fn block_pairs(slots: &[Option<Player>], list: &[u32], blocks: &HashMap<u32, Block>, s: &mut Scratch) {
    let Scratch { bpairs, bstart, bnext, bfrom, lmembers, .. } = s;
    bpairs.clear();
    bstart.clear();
    bnext.clear();
    bfrom.clear();
    if blocks.is_empty() {
        return;
    }
    let of = |i: u32| player(slots, list[i as usize]).and_then(|p| blocks.get(&p.vip));
    bfrom.extend((0..list.len() as u32).filter(|&i| of(i).is_some()));
    if bfrom.is_empty() {
        return;
    }
    // the players they block, by name and by keyphrase, sorted
    let names: HashSet<&str> = bfrom.iter().filter_map(|&i| of(i)).flat_map(|b| b.names.iter().map(String::as_str)).collect();
    let owners: HashSet<&str> = bfrom.iter().filter_map(|&i| of(i)).flat_map(|b| b.owners.iter().map(|o| &**o)).collect();
    let (mut by_name, mut by_owner) = (Vec::new(), Vec::new());
    for (j, &q) in list.iter().enumerate() {
        let Some(q) = player(slots, q) else { continue };
        if names.contains(q.canon.as_str()) {
            by_name.push((q.canon.as_str(), j as u32));
        }
        if let Some(o) = q.owner.as_deref().filter(|o| owners.contains(o)) {
            by_owner.push((o, j as u32));
        }
    }
    by_name.sort_unstable();
    by_owner.sort_unstable();
    let with = |v: &[(&str, u32)], key: &str| -> Vec<u32> {
        v[v.partition_point(|e| e.0 < key)..].iter().take_while(|e| e.0 == key).map(|e| e.1).collect()
    };
    for &i in bfrom.iter() {
        let Some(b) = of(i) else { continue };
        let found = b.names.iter().flat_map(|n| with(&by_name, n)).chain(b.owners.iter().flat_map(|o| with(&by_owner, o)));
        bpairs.extend(found.filter(|&j| j != i).map(|j| (i.min(j), i.max(j))));
    }
    // a lobby's host and whoever is kept apart from one of its players
    let direct = bpairs.len();
    for &(h, m) in lmembers.iter() {
        for k in 0..direct {
            let x = match bpairs[k] {
                (a, b) if a == m => b,
                (a, b) if b == m => a,
                _ => continue,
            };
            if x != h && !lmembers.contains(&(h, x)) {
                bpairs.push((h.min(x), h.max(x)));
            }
        }
    }
    if bpairs.is_empty() {
        return;
    }
    bpairs.sort_unstable();
    bpairs.dedup();
    // each place's others, by place
    bstart.resize(list.len() + 1, 0);
    for &(a, b) in bpairs.iter() {
        bstart[a as usize + 1] += 1;
        bstart[b as usize + 1] += 1;
    }
    for i in 0..list.len() {
        bstart[i + 1] += bstart[i];
    }
    bnext.resize(bstart[list.len()] as usize, 0);
    let mut at: Vec<u32> = bstart[..list.len()].to_vec();
    for &(a, b) in bpairs.iter() {
        for (x, y) in [(a, b), (b, a)] {
            bnext[at[x as usize] as usize] = y;
            at[x as usize] += 1;
        }
    }
}

/// whether places a and b of the room are kept apart (`block_pairs`)
#[inline]
fn kept_apart(bstart: &[u32], bnext: &[u32], a: usize, b: usize) -> bool {
    !bstart.is_empty() && bnext[bstart[a] as usize..bstart[a + 1] as usize].contains(&(b as u32))
}

/// The crews' pairs, linked after the race lobbies' and before any other: two players of the room who share a crew
/// (any of their crews: each pair once), nearest first, into `s.cpairs` as places in the room, the lower first; the
/// link functions make them while both ends have room, each player with at most CREW_LINKS of them.  By distance alone
/// a crew-mate 300 m off in a crowd was never linked (32 nearer players were, 8 under the busy cap), so its car, its
/// name and its arrow never showed.  The rest of the links come in the order of before: a crew-mate near enough is
/// linked by distance anyway, only sooner.
///
/// "Nearest" here is the distance of the two places soon_ms on as the links weigh it (`s.soon`, where the links weigh
/// first: a car moved on a second, a player not in free roam away_m up, so a crew-mate driving comes before one in a
/// shop, and one on its road before one on a road above it), compared as its square: these pairs need no order but
/// their own, and four Math.hypot a pair (`apart`) would cost more than the rest of this.  The order decides only which of a
/// player's crew-mates are linked first when it has more of them than CREW_LINKS, or than room; a pair linked at the
/// last tick weighs as the links do (younger than hold_ms first, older at keep its distance), so that a crew of more
/// than CREW_LINKS driving about does not trade its links every tick.
///
/// What it costs is the crews' pairs, not the room's: the room's players in crews, sorted by crew (one flat list, no
/// map, kept from tick to tick), then every pair of each crew.  A crew of k players in the room is k (k - 1) / 2
/// pairs, so one of more than CREW_GROUP_MAX (2000 players in one crew would be two million pairs) gets none: its
/// players link by distance as everyone does.  Returns how many crews of the room were too large.  (Only while a
/// player is in a crew, and not inlined into the tick, so that a tick without crews is the tick of before.)
#[inline(never)]
fn crew_pairs(slots: &[Option<Player>], list: &[u32], w: &Weigh, s: &mut Scratch) -> u64 {
    let Scratch { soon, local: at, cpairs, cmembers, cmark, csince, .. } = s;
    cpairs.clear();
    cmembers.clear();
    for (j, &q) in list.iter().enumerate() {
        if let Some(p) = player(slots, q) {
            cmembers.extend(crews_of(p).map(|c| (c, j as u32)));
            // (friends: a crew of two each, `friend_id`)
            cmembers.extend(p.friend_ids.iter().map(|&f| (f, j as u32)));
        }
    }
    // by crew, each crew's players by place (each (crew, place) once: a player's crews are distinct)
    cmembers.sort_unstable();
    cmark.clear();
    cmark.resize(list.len(), 0);
    csince.resize(list.len(), 0.0);
    let keep2 = w.keep * w.keep;
    let (mut stamp, mut skipped, mut g) = (0u32, 0, 0);
    while g < cmembers.len() {
        let crew = cmembers[g].0;
        let e = g + cmembers[g..].iter().position(|m| m.0 != crew).unwrap_or(cmembers.len() - g);
        if e - g > CREW_GROUP_MAX {
            skipped += 1;
            g = e;
            continue;
        }
        for x in g..e - 1 {
            let i = cmembers[x].1 as usize;
            // i's links of the last tick, by place
            stamp += 1;
            for l in player(slots, list[i]).map_or(&[][..], |a| &a.links) {
                if let Some(j) = local(at, l.slot) {
                    cmark[j] = stamp;
                    csince[j] = l.since;
                }
            }
            for &(_, j) in &cmembers[x + 1..e] {
                let j = j as usize;
                let sq = square(&soon[i], &soon[j], &w.level);
                let d = if cmark[j] != stamp {
                    sq
                } else if w.now - csince[j] < w.hold_ms {
                    -1.0
                } else {
                    sq * keep2
                };
                cpairs.push((order_key(d), i as u32, j as u32));
            }
        }
        g = e;
    }
    // nearest first, at one place in the order by the places; a pair of two crews once
    cpairs.sort_unstable();
    cpairs.dedup();
    skipped
}

/// a crew's count in `Hub::crew_counts`'s table (0: none)
pub(crate) fn crew_count(counts: &[(u32, u32)], crew: u32) -> usize {
    counts.binary_search_by_key(&crew, |e| e.0).map_or(0, |k| counts[k].1 as usize)
}

/// The links of a room of at most `exact_room` players: every pair of them, nearest first (a link younger than
/// `hold_ms` before them all, an older one at `keep` its distance), into the links while both ends have room.  The
/// outruns' links, the race lobbies' and the crews' come first; returns how many of each were made.
fn link_every_pair(slots: &[Option<Player>], list: &[u32], w: &Weigh, s: &mut Scratch) -> (u64, u64, u64) {
    let m = list.len();
    let Scratch {
        soon,
        ahead,
        view,
        deg,
        ccap,
        mark,
        since,
        pairs,
        order,
        sorting,
        local: at,
        fresh,
        dpairs,
        lpairs,
        cpairs,
        ccount,
        npre,
        bstart,
        bnext,
        ..
    } = s;
    // the outruns' links first, then the race lobbies', then the crews'
    let linked = |fresh: &[Vec<Link>], a: u32, b: u32| fresh[a as usize].iter().any(|l| l.slot == b);
    let (mut duel, mut made) = (0, 0);
    for (k, &(a, b)) in dpairs.iter().chain(lpairs.iter()).enumerate() {
        let (a, b) = (a as usize, b as usize);
        let (sa, sb) = (list[a], list[b]);
        if fresh[sa as usize].len() < deg[a]
            && fresh[sb as usize].len() < deg[b]
            && !linked(fresh, sa, sb)
            && !kept_apart(bstart, bnext, a, b)
        {
            join(fresh, sa, sb, since_of(slots, sa, sb, w.now));
            if k < dpairs.len() {
                duel += 1;
            } else {
                made += 1;
            }
        }
    }
    let mut crew = 0;
    if !cpairs.is_empty() {
        ccount.clear();
        ccount.resize(m, 0);
        for &(_, a, b) in cpairs.iter() {
            let (a, b) = (a as usize, b as usize);
            let (sa, sb) = (list[a], list[b]);
            if ccount[a] < ccap[a]
                && ccount[b] < ccap[b]
                && fresh[sa as usize].len() < deg[a]
                && fresh[sb as usize].len() < deg[b]
                && !linked(fresh, sa, sb)
                && !kept_apart(bstart, bnext, a, b)
            {
                join(fresh, sa, sb, since_of(slots, sa, sb, w.now));
                ccount[a] += 1;
                ccount[b] += 1;
                crew += 1;
            }
        }
    }
    // a pair linked already is one of the links made first, and those are the first of each player's new links: only
    // they are looked through (a few a player), not all of its links
    let first = duel + made + crew > 0;
    if first {
        npre.clear();
        npre.extend(list.iter().map(|&slot| fresh[slot as usize].len() as u32));
    }
    let made_first = |fresh: &[Vec<Link>], a: usize, sa: u32, sb: u32| fresh[sa as usize][..npre[a] as usize].iter().any(|l| l.slot == sb);
    pairs.clear();
    order.clear();
    mark.clear();
    mark.resize(m, 0);
    since.resize(m, 0.0);
    for (i, &slot) in list.iter().enumerate() {
        let Some(a) = player(slots, slot) else { continue };
        // a's links of the last tick, by place
        let stamp = i as u32 + 1;
        for l in &a.links {
            if let Some(j) = local(at, l.slot) {
                mark[j] = stamp;
                since[j] = l.since;
            }
        }
        for j in i + 1..m {
            let linked = mark[j] == stamp;
            let d = if !linked {
                apart(soon, ahead, view, i, j, &w.level)
            } else if w.now - since[j] < w.hold_ms {
                -1.0
            } else {
                apart(soon, ahead, view, i, j, &w.level) * w.keep
            };
            order.push((order_key(d), pairs.len() as u32));
            pairs.push(Pair { since: if linked { since[j] } else { w.now }, a: i as u32, b: j as u32 });
        }
    }
    // a stable sort by d: at one d, the pairs in the order they were weighed
    sort_stable(order, sorting);
    // a link takes two with room: once fewer than two have it, the rest of the pairs make none
    let mut open = (0..m).filter(|&i| fresh[list[i] as usize].len() < deg[i]).count();
    for &(_, k) in order.iter() {
        if open < 2 {
            break;
        }
        let p = &pairs[k as usize];
        let (a, b) = (p.a as usize, p.b as usize);
        let (sa, sb) = (list[a], list[b]);
        if fresh[sa as usize].len() < deg[a]
            && fresh[sb as usize].len() < deg[b]
            && (!first || !made_first(fresh, a, sa, sb))
            && !kept_apart(bstart, bnext, a, b)
        {
            join(fresh, sa, sb, p.since);
            open -= (fresh[sa as usize].len() == deg[a]) as usize + (fresh[sb as usize].len() == deg[b]) as usize;
        }
    }
    (duel, made, crew)
}

/// whether `places` has `j` (every one compared, without a branch: there are at most a degree of them, and this
/// vectorizes)
fn has_place(places: &[u32], j: u32) -> bool {
    places.iter().fold(false, |found, &x| found | (x == j))
}

/// a room at least this large reads its walks on more than the hub's thread (a smaller one's take well under a ms)
const WALKS_SHARED: usize = 1024;

/// a share of a room's places whose walks are read on one thread (`Walks::read`)
struct Part<'p> {
    first: usize,
    pre: &'p mut [(f64, u32)],
    pre_n: &'p mut [u16],
    found: Vec<Found>,
}

/// a part's walks (soon, ahead, from the view point) and the kept vectors it did not take
type PartDone = ([Vec<Near>; 3], Vec<Found>);

/// The walks of a large room's players (`link_many`), read ahead: each player's walks give their nearest ones in an
/// order of their own, whatever the links make of them (a walk reads only the places), so the first ones of every
/// walk can be read before the links are made, on several threads, and the links take them from there in the order
/// they would have read them themselves.  Each walk is read as far as `want` says (`link_many`: as far as it went at
/// the last tick, and a little more); one that needs more goes on from there on the hub's thread, in the grids the
/// links have taken the full players out of by then.
struct Walks<'a> {
    grid: &'a Grid,
    grid_a: &'a Grid,
    soon: &'a [Place],
    ahead: &'a [Place],
    view: &'a [Place],
    /// how far each walk is read ahead (3 a place, at least 1: the links take each walk's nearest one first), and the
    /// most of them
    want: &'a [u16],
    n: usize,
}

impl Walks<'_> {
    /// how far walk k (3 i + w) is read ahead
    fn wanted(&self, k: usize) -> usize {
        self.want[k] as usize
    }

    /// every place's three walks, in `near`, `near_a` and `near_v` by place, and what each gave so far in `pre` and
    /// `pre_n`; from the view point only where it is not the place soon (else it gives what the first walk gives, and
    /// the links never take it)
    #[allow(clippy::too_many_arguments)]
    fn read(
        &self,
        threads: usize,
        found: &mut Vec<Found>,
        near: &mut Vec<Near>,
        near_a: &mut Vec<Near>,
        near_v: &mut Vec<Near>,
        pre: &mut Vec<(f64, u32)>,
        pre_n: &mut Vec<u16>,
    ) {
        let m = self.soon.len();
        near.clear();
        near_a.clear();
        near_v.clear();
        pre.clear();
        pre.resize(m * 3 * self.n, (0.0, 0));
        pre_n.clear();
        pre_n.resize(m * 3, 0);
        let threads = if m >= WALKS_SHARED { threads.max(1) } else { 1 };
        // parts, four a thread: one thread held up (a busy machine) holds up a little of the walks only, as the others
        // take the parts left
        let per = m.div_ceil(if threads == 1 { 1 } else { 4 * threads }).max(1);
        let parts: Vec<Mutex<Option<Part>>> = pre
            .chunks_mut(per * 3 * self.n)
            .zip(pre_n.chunks_mut(per * 3))
            .enumerate()
            .map(|(t, (pre, pre_n))| {
                // the vectors kept from the last tick, a share to each part
                let found = found.split_off(found.len() - found.len().min(3 * per));
                Mutex::new(Some(Part { first: t * per, pre, pre_n, found }))
            })
            .collect();
        let done: Vec<Mutex<Option<PartDone>>> = parts.iter().map(|_| Mutex::new(None)).collect();
        let next = AtomicUsize::new(0);
        let work = || {
            loop {
                let t = next.fetch_add(1, Ordering::Relaxed);
                let Some(slot) = parts.get(t) else { break };
                let Some(mut p) = slot.lock().unwrap_or_else(|e| e.into_inner()).take() else { continue };
                let walks = self.part(p.first, p.pre, p.pre_n, &mut p.found);
                *done[t].lock().unwrap_or_else(|e| e.into_inner()) = Some((walks, p.found));
            }
        };
        if threads == 1 {
            work();
        } else {
            let work = &work;
            std::thread::scope(|sc| {
                for _ in 1..threads {
                    // (a thread not to be had: the others take its parts)
                    let _ = std::thread::Builder::new().name("roam-links".into()).spawn_scoped(sc, work);
                }
                work();
            });
        }
        for d in done {
            let Some(([s, a, v], f)) = d.into_inner().unwrap_or_else(|e| e.into_inner()) else { continue };
            near.extend(s);
            near_a.extend(a);
            near_v.extend(v);
            found.extend(f);
        }
    }

    /// places from `first` on, as many as `pre_n` has room for (3 a place)
    fn part(&self, first: usize, pre: &mut [(f64, u32)], pre_n: &mut [u16], found: &mut Vec<Found>) -> [Vec<Near>; 3] {
        let count = pre_n.len() / 3;
        let mut out = [Vec::with_capacity(count), Vec::with_capacity(count), Vec::with_capacity(count)];
        for k in 0..count {
            let i = first + k;
            let mut walks = [
                Near::new(self.grid, self.soon, i as u32, found.pop().unwrap_or_default()),
                Near::new(self.grid_a, self.ahead, i as u32, found.pop().unwrap_or_default()),
                Near::from(self.grid, self.view[i], i as u32, found.pop().unwrap_or_default()),
            ];
            for (w, near) in walks.iter_mut().enumerate() {
                if w == WALK_VIEW as usize && self.view[i] == self.soon[i] {
                    continue;
                }
                let (g, pos) = if w == WALK_AHEAD as usize { (self.grid_a, self.ahead) } else { (self.grid, self.soon) };
                let at = (3 * k + w) * self.n;
                let want = self.wanted(3 * i + w);
                let mut got = 0;
                while got < want {
                    let Some(e) = near.next(g, pos) else { break };
                    pre[at + got] = e;
                    got += 1;
                }
                pre_n[3 * k + w] = got as u16;
            }
            let [s, a, v] = walks;
            out[0].push(s);
            out[1].push(a);
            out[2].push(v);
        }
        out
    }
}

/// The links of a room larger than `exact_room`, as `link_every_pair` makes them from every pair sorted (at one
/// place in the order the pair of the lower index first), without sorting them all: the last tick's links at their own place,
/// and each player's nearest one after another (Near) while it has room, merged in that order.  A player walks three
/// times: by the places soon, by the places ahead, and from its view point over the places soon (`apart` is the
/// nearest of the four distances, a pair's view distance found by either end's walk): a pair comes first at the
/// nearest of its distances, as the order has it, and when it comes again, from another walk, it is linked already
/// or cannot be (the same as an entry of one pair coming twice, below).  A player without a view point (its view
/// point is its place soon) skips the third walk, which would give what its first gives.
///
/// One heap could hold both.  Here the last tick's links (32,000 of 2000 players) are sorted once, and only the
/// nearest ones (one a player) are in the heap, which stays small; the two are merged.  The links come out the same:
/// the pairs are taken in the same order (by distance, then the lower place, then the higher), and of entries at one
/// place in it, which are entries of one pair, the order makes no difference (at most one of them links the pair, the
/// same one whichever comes first; one that comes after the pair filled a player only reads that player's next
/// nearest one, which never links anything, as the player has no room left).
///
/// What the order asks of a pair, which players have room and which pairs were or are linked, is read from arrays
/// by place in the room (a player's last links and its new ones, as places), not from the players.  (Not inlined
/// into the tick: there it made the every-pair loop of the small rooms 1 % slower.)  The outruns' links, the race
/// lobbies' and the crews' come first, as in `link_every_pair`; returns how many of each were made.
///
/// Most of a large room's links are the last tick's again (97 % of 256,000 at 16000 players), and every player's
/// walks have to go past its own links and the full players near it to the next one that can still link: a million
/// of them a tick at 16000 players, nearly all taken only to read the next.  So the walks are read ahead on threads
/// of their own (`Walks`), each nearest one that cannot link any more is passed over as it is read (`advance`), not
/// sent through the heap, and a player whose links are full goes out of the grids, so the walks still to be read
/// pass it by.  The links are the same, byte for byte (tests/roam_hub.rs: on one thread or four); on a Ryzen 9 5900X
/// at 16000 players in one room the hub's thread went from about 1.2 s a tick to 0.4 s.
#[inline(never)]
fn link_many(slots: &[Option<Player>], list: &[u32], w: &Weigh, s: &mut Scratch) -> (u64, u64, u64) {
    let m = list.len();
    let Scratch {
        soon,
        ahead,
        view,
        deg,
        ccap,
        local: at,
        fresh,
        grid,
        heap,
        near,
        grid_a,
        near_a,
        near_v,
        found,
        olds,
        olinks,
        osince,
        ostart,
        pre,
        pre_n,
        pre_at,
        want,
        walk_used,
        fcount,
        flinks,
        dpairs,
        lpairs,
        cpairs,
        ccount,
        bstart,
        bnext,
        ..
    } = s;
    let cand = |d: f64, i: usize, j: usize, nearest: bool| Cand::new(d, i as u32, j as u32, nearest);
    grid.build(soon, w.level);
    grid_a.build(ahead, w.level);
    heap.clear();
    olds.clear();
    olinks.clear();
    osince.clear();
    ostart.clear();
    // every player's walks read ahead (on threads of their own in a large room): as far as each one went at the last
    // tick and a quarter more, its links at most (a walk new to the room, its links).  How far they are read makes no
    // difference to the links, only to the threads' share of the work: most walks go 5 to 15, a few hundreds (a
    // player looking for one more link in a crowd of full ones), and which ones those are changes from tick to tick.
    want.clear();
    for (i, &slot) in list.iter().enumerate() {
        let most = deg[i].max(1);
        for went in walk_used.get(slot as usize).copied().unwrap_or([0; 3]) {
            let v = match went {
                0 => most,
                n => n as usize - 1 + 2 + (n as usize - 1) / 4,
            };
            want.push(v.clamp(1, most) as u16);
        }
    }
    let ahead_n = want.iter().copied().max().unwrap_or(1) as usize;
    Walks { grid, grid_a, soon, ahead, view, want, n: ahead_n }.read(w.threads, found, near, near_a, near_v, pre, pre_n);
    pre_at.clear();
    pre_at.resize(m * 3, 0);
    for (i, &slot) in list.iter().enumerate() {
        ostart.push(olinks.len() as u32);
        if let Some(a) = player(slots, slot) {
            for l in &a.links {
                let Some(j) = local(at, l.slot) else { continue };
                olinks.push(j as u32);
                osince.push(l.since);
                // each link once: by its lower end, or by the only end that lists it (j's links are in already)
                if i < j || !has_place(&olinks[ostart[j] as usize..ostart[j + 1] as usize], i as u32) {
                    // a link's place in the order: younger than `hold_ms` first, older at `keep` its distance
                    let d = if w.now - l.since < w.hold_ms { -1.0 } else { apart(soon, ahead, view, i, j, &w.level) * w.keep };
                    olds.push(cand(d, i, j, false));
                }
            }
        }
    }
    ostart.push(olinks.len() as u32);
    olds.sort_unstable_by_key(|c| (order_key(c.d), c.pair()));
    let old_of = |i: usize| &olinks[ostart[i] as usize..ostart[i + 1] as usize];
    // linked at the last tick, at either end
    let old = |i: usize, j: usize| has_place(old_of(i), j as u32) || has_place(old_of(j), i as u32);
    // each place's new links so far, as places (`stride` a place, its degree at most)
    let stride = deg.iter().copied().max().unwrap_or(0);
    fcount.clear();
    fcount.resize(m, 0);
    flinks.clear();
    flinks.resize(m * stride, 0);
    let room = |fcount: &[u32], i: usize| (fcount[i] as usize) < deg[i];
    // the outruns' links first, then the race lobbies', then the crews'
    let (mut duel, mut made) = (0, 0);
    for (k, &(a, b)) in dpairs.iter().chain(lpairs.iter()).enumerate() {
        let (i, j) = (a as usize, b as usize);
        if room(fcount, i)
            && room(fcount, j)
            && !has_place(&flinks[i * stride..i * stride + fcount[i] as usize], b)
            && !kept_apart(bstart, bnext, i, j)
        {
            join(fresh, list[i], list[j], since_of(slots, list[i], list[j], w.now));
            for (a, b) in [(i, j), (j, i)] {
                flinks[a * stride + fcount[a] as usize] = b as u32;
                fcount[a] += 1;
            }
            if k < dpairs.len() {
                duel += 1;
            } else {
                made += 1;
            }
        }
    }
    let mut crew = 0;
    if !cpairs.is_empty() {
        ccount.clear();
        ccount.resize(m, 0);
        for &(_, a, b) in cpairs.iter() {
            let (i, j) = (a as usize, b as usize);
            if ccount[i] < ccap[i]
                && ccount[j] < ccap[j]
                && room(fcount, i)
                && room(fcount, j)
                && !has_place(&flinks[i * stride..i * stride + fcount[i] as usize], b)
                && !kept_apart(bstart, bnext, i, j)
            {
                join(fresh, list[i], list[j], since_of(slots, list[i], list[j], w.now));
                for (a, b) in [(i, j), (j, i)] {
                    flinks[a * stride + fcount[a] as usize] = b as u32;
                    fcount[a] += 1;
                    ccount[a] += 1;
                }
                crew += 1;
            }
        }
    }
    // Walk w of place i on to its next one that can still link: read ahead, or on from where the reading stopped
    // (`pre_at` counts what it gave, read ahead or not: the next tick reads that far ahead).  One that cannot link is
    // passed over at once, where the order would have taken it only to read the walk's next one: the other player
    // full, the pair linked already, a link of the last tick (taken at its own place, `olds`) or kept apart.  None of
    // that changes later in the tick (no player gets room back, no link goes), so the links are the same, and a
    // player looking for one more link in a crowd of full ones no longer sends each of them through the heap.
    macro_rules! advance {
        ($i:expr, $walk:expr) => {{
            let (i, walk): (usize, u8) = ($i, $walk);
            let k = i * 3 + walk as usize;
            loop {
                let (got, read) = (pre_at[k] as usize, pre_n[k] as usize);
                pre_at[k] = pre_at[k].saturating_add(1);
                let e = if got < read {
                    Some(pre[k * ahead_n + got])
                } else if read == want[k] as usize {
                    // read as far as it was to be and wanting more: the walk goes on from there
                    match walk {
                        WALK_AHEAD => near_a[i].next(grid_a, ahead),
                        WALK_VIEW => near_v[i].next(grid, soon),
                        _ => near[i].next(grid, soon),
                    }
                } else {
                    // the walk ended
                    None
                };
                let Some((d, j)) = e else { break None };
                let jl = j as usize;
                if !room(fcount, jl)
                    || old(i, jl)
                    || has_place(&flinks[i * stride..i * stride + fcount[i] as usize], j)
                    || kept_apart(bstart, bnext, i, jl)
                {
                    continue;
                }
                break Some((d, j));
            }
        }};
    }
    // a player with no room left (none at all, or filled by the lobbies and crews) out of the grids: the walks read
    // from here on pass it by (`advance` would pass it over), and so on as each one fills
    for i in 0..m {
        if !room(fcount, i) {
            grid.remove(i as u32);
            grid_a.remove(i as u32);
        }
    }
    // each walk's first one (as the walks were read: soon, ahead, then from the view point), but none for a player
    // with no room left (the order would have taken it only to find that)
    for i in 0..m {
        if room(fcount, i) {
            for walk in [WALK_SOON, WALK_AHEAD, WALK_VIEW] {
                if let Some((d, j)) = advance!(i, walk) {
                    heap.push(Cand::by(d, i as u32, j, walk));
                }
            }
        }
    }
    let mut o = 0;
    loop {
        // the next in the order: the heap's nearest one or the next link of the last tick
        let take_old = match (olds.get(o), heap.peek()) {
            (None, None) => break,
            (Some(x), Some(y)) => !before(y, x),
            (Some(_), None) => true,
            (None, Some(_)) => false,
        };
        let c = if take_old {
            o += 1;
            olds[o - 1]
        } else {
            match heap.pop() {
                Some(c) => c,
                None => break,
            }
        };
        let (i, j) = (c.i() as usize, c.j() as usize);
        // i's next one of that walk, while it has room (a link of the last tick is never one: `advance`)
        if c.nearest()
            && room(fcount, i)
            && let Some((d, k)) = advance!(i, c.walk())
        {
            heap.push(Cand::by(d, i as u32, k, c.walk()));
        }
        if room(fcount, i)
            && room(fcount, j)
            && !has_place(&flinks[i * stride..i * stride + fcount[i] as usize], j as u32)
            && !kept_apart(bstart, bnext, i, j)
        {
            // a nearest one is linked since now, a link of the last tick since it was made (one of i's links)
            let since = if c.nearest() {
                w.now
            } else {
                // its first in i's links, as `olinks` has them in the same order (all but those out of the room)
                let (a, z) = (ostart[i] as usize, ostart[i + 1] as usize);
                olinks[a..z].iter().position(|&x| x as usize == j).map_or(w.now, |k| osince[a + k])
            };
            join(fresh, list[i], list[j], since);
            for (a, b) in [(i, j), (j, i)] {
                flinks[a * stride + fcount[a] as usize] = b as u32;
                fcount[a] += 1;
                // full now: out of the walks still to be read
                if !room(fcount, a) {
                    grid.remove(a as u32);
                    grid_a.remove(a as u32);
                }
            }
        }
    }
    // how far each walk went, for the next tick's read ahead (by slot, 0: not known)
    walk_used.resize(slots.len(), [0; 3]);
    for (i, &slot) in list.iter().enumerate() {
        let used = |w: usize| pre_at[3 * i + w].saturating_add(1);
        walk_used[slot as usize] = [used(0), used(1), used(2)];
    }
    for n in near.drain(..).chain(near_a.drain(..)).chain(near_v.drain(..)) {
        found.push(n.into_found());
    }
    (duel, made, crew)
}

/// whether one of the two players' browsers blocks the other (`Hub::blocks`)
fn blocked(blocks: &HashMap<u32, Block>, a: &Player, b: &Player) -> bool {
    blocks.get(&a.vip).is_some_and(|k| k.has(b)) || blocks.get(&b.vip).is_some_and(|k| k.has(a))
}

/// where a player is at `now` (x, y): its last SNAP's position moved on at its velocity, MAP_AHEAD_MS at most
fn ahead(o: &Player, now: f64) -> [f64; 2] {
    ahead_of(o.p.unwrap_or_default(), [o.v[0], o.v[1]], o.pos_at, now)
}

/// `ahead` of a player at `pos` at `pos_at`, going at `v`
fn ahead_of(pos: [f64; 3], v: [f64; 2], pos_at: f64, now: f64) -> [f64; 2] {
    let dt = (now - pos_at).clamp(0.0, MAP_AHEAD_MS) / 1000.0;
    [pos[0] + v[0] * dt, pos[1] + v[1] * dt]
}

/// a player in `slot` as a map places it, at `at` (`ahead`): whole metres, its velocity in whole m/s; parked if it
/// has stood at one spot for PARKED_MS by `now`
fn map_cand(slot: u32, o: &Player, at: [f64; 2], now: f64) -> MapCand {
    map_cand_of(slot, o.id, o.p.unwrap_or_default(), o.heading, [o.v[0], o.v[1]], at, now - o.stay_at >= PARKED_MS)
}

/// `map_cand` of a player with that id, position, heading and velocity
fn map_cand_of(slot: u32, id: u32, p: [f64; 3], heading: u8, v: [f64; 2], at: [f64; 2], parked: bool) -> MapCand {
    let c = |x: f64| (x + 0.5).floor().clamp(-32767.0, 32767.0) as i16;
    let w = |x: f64| x.round().clamp(-127.0, 127.0) as i8;
    MapCand { slot, id, p, x: c(at[0]), y: c(at[1]), heading, vx: w(v[0]), vy: w(v[1]), parked }
}

/// the NEARD grid's cell of a coordinate (m)
fn near_cell(x: f64) -> i32 {
    (x / NEAR_CELL_M).floor().clamp(-1e6, 1e6) as i32
}

/// MAPD (and NEARD): the entries that take a map from `from` (by slot) to `list` (by slot): each player come, or moved
/// off where the page has it now (tol_m metres, or its heading tol_h), placed, each one gone gone; with where each entry
/// ends in the bytes, and what the map holds after them (by slot).  With `parked`, a parked player's key has
/// wire::MAPD_PARKED (a slot under it: every one), one parked since or no longer is placed again, and each placed
/// one's height follows (wire::MAPD_PLACE_Z).
fn map_diff(from: &[MapSent], list: &[MapCand], now: f64, tol_m: f64, tol_h: u8, parked: bool) -> (Vec<u8>, Vec<usize>, Vec<MapSent>) {
    let (mut e, mut ends, mut next) = (Vec::new(), Vec::new(), Vec::with_capacity(list.len()));
    let pk = |c: &MapCand| parked && c.parked && c.slot < wire::MAPD_PARKED as u32;
    let place = |e: &mut Vec<u8>, c: &MapCand| {
        e.extend_from_slice(&(c.slot as u16 | if pk(c) { wire::MAPD_PARKED } else { 0 }).to_le_bytes());
        e.extend_from_slice(&c.x.to_le_bytes());
        e.extend_from_slice(&c.y.to_le_bytes());
        e.extend_from_slice(&[c.heading, c.vx as u8, c.vy as u8]);
        if parked {
            e.extend_from_slice(&((c.p[2] + 0.5).floor().clamp(-32767.0, 32767.0) as i16).to_le_bytes());
        }
        MapSent { slot: c.slot, id: c.id, x: c.x, y: c.y, heading: c.heading, vx: c.vx, vy: c.vy, parked: pk(c), t: now }
    };
    let (mut i, mut j) = (0, 0);
    while i < from.len() || j < list.len() {
        let (a, c) = (from.get(i), list.get(j));
        let was = e.len();
        match (a, c) {
            (Some(a), Some(c)) if a.slot == c.slot => {
                let dt = (now - a.t) / 1000.0;
                let (dx, dy) = (c.x as f64 - (a.x as f64 + a.vx as f64 * dt), c.y as f64 - (a.y as f64 + a.vy as f64 * dt));
                let dh = c.heading.wrapping_sub(a.heading).min(a.heading.wrapping_sub(c.heading));
                next.push(if a.id != c.id || dx * dx + dy * dy > tol_m * tol_m || dh > tol_h || a.parked != pk(c) {
                    place(&mut e, c)
                } else {
                    *a
                });
                (i, j) = (i + 1, j + 1);
            }
            (Some(a), c) if c.is_none_or(|c| a.slot < c.slot) => {
                e.extend_from_slice(&(a.slot as u16 | wire::MAPD_GONE).to_le_bytes());
                i += 1;
            }
            (_, Some(c)) => {
                next.push(place(&mut e, c));
                j += 1;
            }
            _ => break,
        }
        if e.len() > was {
            ends.push(e.len());
        }
    }
    (e, ends, next)
}

/// MAPD's datagrams for entries ending at `ends`: each its first entry and the one past its last, MAPD_BYTES of
/// entries and 255 entries at most a datagram; one empty for none
fn map_parts(ends: &[usize]) -> Vec<(usize, usize)> {
    let mut parts = vec![(0, 0)];
    for (k, &end) in ends.iter().enumerate() {
        let (a, z) = *parts.last().unwrap();
        let start = if a == 0 { 0 } else { ends[a - 1] };
        if z > a && (end - start > wire::MAPD_BYTES || z - a == 255) {
            parts.push((k, k + 1));
        } else {
            parts.last_mut().unwrap().1 = k + 1;
        }
    }
    parts
}
