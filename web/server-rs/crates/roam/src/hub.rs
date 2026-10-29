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

use std::sync::Arc;

use crate::links::{Cand, Found, Grid, Heap, Near, WALK_AHEAD, WALK_SOON, WALK_VIEW, before, distance, order_key, sort_stable};
use crate::results::{self, Results};
use crate::watch::{self, Seen, Watch};
use crate::wire::{self, F_DATA, KIND_UDP, Msg};
use crate::{PeerEntry, ROAM_HUB_VIP, RoamOptions};

/// roam_core.h RC_PEERS_V1: what a core of version 1 reads (its group was 8, and it drops a longer list)
const DEGREE_V1: usize = 7;
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
/// Two players of a room who share a crew are linked after the race lobbies' links and before all others, each
/// player with at most this many such links (`crew_pairs`): half of the 32, so that the cars around it still show
/// (under the busy cap, 8 links a player, a crew of 9 or more in one place takes all of them)
const CREW_LINKS: u32 = 16;
/// a crew of more players than this in one room gets no links first (its pairs grow as the square of its players:
/// 64 are 2016), its players link by distance as everyone does, and it counts in `crewSkips`
const CREW_GROUP_MAX: usize = 64;

/// A player's race report as a log line (one each player of a race, back in free roam: the lines of one race share
/// its lobby), for evening out the cars: the race, how the player did, the car as its game rated it.  Grep `[race]`.
fn race_line(player: &str, r: &wire::RaceRep) -> String {
    let mode = results::mode_name(r.mode);
    let how = match r.place {
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
    };
    let time = if r.time > 0.0 { format!(" in {:.2} s", r.time) } else { String::new() };
    let name: String = r.name.iter().map(|&c| c as char).collect();
    format!(
        "[race] lobby {:08x}, race {:08x} ({mode}, track {}, {} lap{}, {} cars): {player}{} {how}{time}; {name} (type {}), \
         class {}, rating {:.1} (the cars' average {:.1}), {:.0} kW, {:.0} kg, {:.0} km/h, power x{:.3}",
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
        r.factor
    )
}

/// how often a player's claim hears that it plays (the claim holds 180 days from its last game: an hour is plenty)
const SEEN_EVERY_MS: f64 = 600_000.0;

/// a link: the player it is to, and since when
#[derive(Clone, Copy, Debug)]
struct Link {
    slot: u32,
    since: f64,
}

struct Player {
    vip: u32,
    port: u16,
    /// the hub's port it last wrote to (the source port of its list)
    hub_port: u16,
    id: u32,
    room: u32,
    /// as the player last sent it (roam_core.h RC_ST_*: 2 in free roam, 5 paused there): the world map and
    /// `in_roam()` read it
    status: u8,
    /// kept as the player last sent it (its race reports' lines name it)
    name: String,
    /// its name's Online form in lower case (nfsu2-claims `canonical`; empty: nothing of a name): blocks go by it
    canon: String,
    degree: usize,
    p: Option<[f64; 3]>,
    /// its velocity and heading (256 a turn) as of the position
    v: [f64; 3],
    heading: u8,
    pos_at: f64,
    seen: f64,
    /// its HELLO says it takes race lobbies (its map questions get the room's lobbies too)
    lobbies: bool,
    /// its crews as its last HELLO named them (wire::HF_CREW), each once, 0 after them: its crew-mates of its room are
    /// linked with it first (`crew_pairs`), and /players counts them (`crew_counts`)
    crews: [u32; wire::CREWS],
    /// its HELLO says it reads the crew marks of our lists (wire::HF_HUBCREW): its lists end in them while it is in
    /// a crew (`lists`)
    marks: bool,
    /// the race lobby it hosts, as its last advert to the hub had it, and when that came
    lobby: Option<(HubLobby, f64)>,
    /// the lobby it asked to join (its id), and when
    join: Option<(u32, f64)>,
    /// the lobby of the race it last reported, and when (one report a race is logged)
    reported: Option<(u32, f64)>,
    /// its links after the last tick (a HELLO of another start of the game empties them)
    links: Vec<Link>,
    /// how its car moves (watch.rs)
    watch: Watch,
    /// its place in `order`
    at: u32,
    /// the keyphrase its page plays with (nfsu2-claims' owner: its hello to the switch named it), if any
    owner: Option<Arc<str>>,
    /// its name is claimed by another keyphrase (nfsu2-claims): it gets no links, shows on no map and counts nowhere,
    /// as of the claims' `generation` in `checked` (0: not looked at yet)
    impostor: bool,
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
        degree: n(o.degree, d.degree),
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
        // (one answer is three datagrams at most)
        map_max: n(o.map_max, d.map_max).min(wire::MAP_PER * 3),
        map_gap_ms: f(o.map_gap_ms, d.map_gap_ms),
        map_fresh_ms: f(o.map_fresh_ms, d.map_fresh_ms),
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
    soon: Vec<[f64; 3]>,
    ahead: Vec<[f64; 3]>,
    view: Vec<[f64; 3]>,
    deg: Vec<usize>,
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
    /// each place's links of the last tick as places in the room: `olinks[ostart[i]..ostart[i + 1]]`
    olinks: Vec<u32>,
    ostart: Vec<u32>,
    /// each place's new links as places (`flinks[i * stride..][..fcount[i]]`)
    fcount: Vec<u32>,
    flinks: Vec<u32>,
    /// the block the lists are cut from (`lists`)
    buf: BytesMut,
}

/// The bytes of a block of lists: what a list waiting in a slow player's queue keeps at most
const LIST_BLOCK: usize = 4096;

/// what the links of a room are weighed by
struct Weigh {
    now: f64,
    hold_ms: f64,
    keep: f64,
}

/// how far apart places i and j of a room count: the nearest of the two soon_ms on, the two look_ms on, and either
/// one's view point to the other soon_ms on
#[inline]
fn apart(soon: &[[f64; 3]], ahead: &[[f64; 3]], view: &[[f64; 3]], i: usize, j: usize) -> f64 {
    distance(&soon[i], &soon[j]).min(distance(&ahead[i], &ahead[j])).min(distance(&view[i], &soon[j])).min(distance(&view[j], &soon[i]))
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
    /// the pairs of players kept apart at the last tick (`block_pairs`), as slots, the lower first, sorted: the world
    /// map and the lobby list read them between ticks
    apart: Vec<(u32, u32)>,
    /// the endpoints of each virtual address (`max_per_vip`)
    per_vip: HashMap<u32, u32>,
    /// the players whose last HELLO named a crew: with none, a tick does not look for crews at all
    crewed: usize,
    /// the cheat watches of players gone without a BYE, by endpoint, for WATCH_KEEP_MS (at most max_players of them)
    kept: HashMap<u64, KeptWatch>,
    /// the races' results as the players' games showed them, until each race is judged (results.rs)
    results: Results,
    /// each virtual address's last world map answered (map_gap_ms), kept past its players: a BYE and a HELLO (or
    /// another port) make a new player, and when a player's own time was the limit, a new player's first answer came
    /// at once (the second security audit, 2026-10-22: a map walks every player on the hub's one thread, and a few
    /// clients asking that way kept it busy); dropped at each tick once older than map_gap_ms
    map_at: HashMap<u32, f64>,
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
            apart: Vec::new(),
            per_vip: HashMap::new(),
            crewed: 0,
            kept: HashMap::new(),
            results: Results::default(),
            map_at: HashMap::new(),
            s: Scratch::default(),
            claims: None,
        }
    }

    /// the claimed names its players' names are checked against (the player's page's keyphrase: `frame_from`)
    pub fn set_claims(&mut self, claims: Option<Arc<nfsu2_claims::Claims>>) {
        self.claims = claims;
    }

    /// a player's name against the claims, when its name, its owner or the claims changed since it was last looked
    /// at: another keyphrase's name makes it an impostor (the console says so once); its own claim hears it plays
    fn check_claim(&mut self, slot: u32, now: f64) {
        let Some(c) = &self.claims else { return };
        let g = c.generation();
        let Some(Some(p)) = self.slots.get_mut(slot as usize) else { return };
        if p.checked != g {
            p.checked = g;
            let was = p.impostor;
            p.impostor = !c.may_use(&p.name, p.owner.as_deref(), now as i64);
            if p.impostor && !was {
                (self.log)(&format!(
                    "[roam] {} plays as \"{}\", a name another keyphrase claimed: no links",
                    nfsu2_util::vip_str(p.vip),
                    js::printable(&p.name, 32)
                ));
            }
        }
        if let Some(o) = p.owner.as_deref().filter(|_| !p.impostor && now - p.seen_claim >= SEEN_EVERY_MS) {
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
                    pos_at: 0.0,
                    seen: 0.0,
                    lobbies: false,
                    crews: [0; wire::CREWS],
                    marks: false,
                    lobby: None,
                    join: None,
                    reported: None,
                    links: Vec::new(),
                    watch: Watch::new((self.now)()),
                    at: 0,
                    owner: owner.cloned(),
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
        let mut report = None;
        let mut seen = [None, None];
        let mut res = None;
        // (in a crew before this HELLO, and after it)
        let mut crewed = None;
        // (a HELLO: its name against the claims)
        let mut check = false;
        match m {
            Msg::Hello { sender, core, status, room, name, lobbies, marks, crews } => {
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
                }
                if p.owner.as_ref() != owner {
                    p.owner = owner.cloned();
                    p.checked = 0;
                }
                check = true;
                p.degree = if core >= 2 { degree } else { degree.min(DEGREE_V1) };
                p.lobbies = lobbies;
                p.marks = marks;
                let was = p.crews[0] != 0;
                p.crews = distinct(crews);
                crewed = Some((was, p.crews[0] != 0));
            }
            Msg::Snap { sender, status, p: pos, v, heading, t_us, clock } if sender == p.id => {
                p.p = Some(pos);
                p.v = v;
                p.heading = heading;
                p.pos_at = p.seen;
                p.status = status;
                if matches!(status, 2 | 5) {
                    seen = p.watch.snap(now, pos, t_us, clock);
                } else {
                    p.watch.reset();
                }
            }
            Msg::MapReq { req } if sender == p.id => ask = Some(req),
            Msg::Lobby { op, to: 0, lobby, race, x, y, n, max, state, ids } if sender == p.id => match op {
                wire::LOBBY_ADVERT => p.lobby = Some((HubLobby { id: lobby, race, x, y, n, max, state, ids }, now)),
                wire::LOBBY_CLOSE if p.lobby.is_some_and(|(l, _)| l.id == lobby) => p.lobby = None,
                wire::LOBBY_JOIN => p.join = Some((lobby, now)),
                _ => {}
            },
            // one report a race (its lobby), and none within REPORT_GAP_MS of the player's last
            Msg::RaceRep(r) if sender == p.id && !p.reported.is_some_and(|(l, t)| l == r.lobby || now - t < REPORT_GAP_MS) => {
                p.reported = Some((r.lobby, now));
                report = Some(race_line(&p.name, &r));
            }
            Msg::RaceRes(r) if sender == p.id => res = Some((p.id, r)),
            _ => {}
        }
        if let Some((was, is)) = crewed {
            self.crewed = self.crewed + is as usize - was as usize;
        }
        if check {
            self.check_claim(slot, now);
        }
        if let Some(line) = report {
            self.st.race_reports += 1;
            (self.log)(&line);
        }
        for s in seen.into_iter().flatten() {
            self.cheat(slot, s, now);
        }
        if let Some((by, r)) = res {
            let mut results = std::mem::take(&mut self.results);
            results.add(now, by, r, &|id| self.who(id));
            self.results = results;
        }
        if let Some(req) = ask {
            self.map(slot, req, now);
        }
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

    /// The world map's answer to the player in `slot` (roam_core.h RC_MSG_MAP): the players of its room in free roam
    /// (or paused there) with a position younger than map_fresh_ms, past itself and its links, nearest first (at one
    /// distance in the order they came), map_max of them at most, in datagrams of MAP_PER; one answer to a browser (its
    /// virtual address, whatever its endpoints) every map_gap_ms at most (a page asks again when an answer does not
    /// come).
    fn map(&mut self, slot: u32, req: u16, now: f64) {
        let (gap, fresh, max) = (self.o.map_gap_ms, self.o.map_fresh_ms, self.o.map_max);
        let Some(p) = self.player(slot) else { return };
        if self.map_at.get(&p.vip).is_some_and(|&t| now - t < gap) {
            return;
        }
        if p.impostor {
            return;
        }
        let (room, at) = (p.room, p.p.unwrap_or_default());
        let mut near: Vec<(u64, (i16, i16, u8))> = Vec::new();
        for &q in &self.order {
            let Some(o) = self.player(q) else { continue };
            let Some(pos) = o.p else { continue };
            if q == slot
                || o.impostor
                || o.room != room
                || now - o.pos_at > fresh
                || !matches!(o.status, 2 | 5)
                || p.links.iter().any(|l| l.slot == q)
            {
                continue;
            }
            if self.apart_from(slot, q, p, o) {
                continue;
            }
            let c = |x: f64| (x + 0.5).floor().clamp(-32767.0, 32767.0) as i16;
            near.push((order_key(distance(&at, &pos)), (c(pos[0]), c(pos[1]), o.heading)));
        }
        let (vip, port, hub_port) = (p.vip, p.port, p.hub_port);
        // (a stable sort: at one distance the order they came)
        near.sort_by_key(|e| e.0);
        let total = near.len().min(u16::MAX as usize) as u16;
        near.truncate(max);
        let list: Vec<(i16, i16, u8)> = near.into_iter().map(|e| e.1).collect();
        let parts = list.len().div_ceil(wire::MAP_PER).max(1);
        for (k, part) in list.chunks(wire::MAP_PER).chain(list.is_empty().then_some(&[][..])).enumerate() {
            let mut f = vec![0u8; F_DATA + wire::map_len(part.len())];
            f[0] = KIND_UDP;
            f[4..8].copy_from_slice(&ROAM_HUB_VIP.to_be_bytes());
            f[8..12].copy_from_slice(&vip.to_be_bytes());
            f[12..14].copy_from_slice(&hub_port.to_be_bytes());
            f[14..16].copy_from_slice(&port.to_be_bytes());
            wire::write_map(&mut f[F_DATA..], self.id, self.seq, req, k as u8, parts as u8, total, part);
            self.seq = self.seq.wrapping_add(1);
            (self.send)(vip, Bytes::from(f));
        }
        self.map_at.insert(vip, now);
        self.lobbies(slot, req, now);
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
        let gap = self.o.map_gap_ms;
        self.map_at.retain(|_, t| now - *t < gap);
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
        // the races whose results all came (or a while ago): the ones won by cheating
        for v in self.results.due(now) {
            let who = |id: u32| v.names.get(&id).cloned().or_else(|| self.who(id)).unwrap_or_else(|| format!("player {id:08x}"));
            let line = results::line(&v, &who);
            self.st.cheat_lines += 1;
            (self.log)(&line);
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

        // the rooms: the players with a position younger than `stale_ms`, in the order they came
        let s = &mut self.s;
        s.room_of.clear();
        let mut rooms = 0;
        for &slot in &self.order {
            let Some(Some(p)) = self.slots.get(slot as usize) else { continue };
            if p.p.is_none() || now - p.pos_at > self.o.stale_ms || p.impostor {
                continue;
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
        let w = Weigh { now, hold_ms: self.o.hold_ms, keep: self.o.keep };
        let (mut lobby_links, mut crew_links, mut crew_skips) = (0, 0, 0);
        for r in 0..rooms {
            let list = std::mem::take(&mut s.rooms[r]);
            prepare(&self.slots, &list, self.cap, &self.o, now, s);
            lobby_pairs(&self.slots, &list, now, s);
            block_pairs(&self.slots, &list, &self.blocks, s);
            self.apart.extend(s.bpairs.iter().map(|&(a, b)| (list[a as usize], list[b as usize])).map(|(x, y)| (x.min(y), x.max(y))));
            // (no player in a crew, nothing to look for)
            if self.crewed > 0 {
                crew_skips += crew_pairs(&self.slots, &list, &w, s);
            } else {
                s.cpairs.clear();
            }
            let (lobby, crew) = if list.len() > self.o.exact_room {
                link_many(&self.slots, &list, &w, s)
            } else {
                link_every_pair(&self.slots, &list, &w, s)
            };
            lobby_links += lobby;
            crew_links += crew;
            for &slot in &list {
                s.local[slot as usize] = NONE;
            }
            s.rooms[r] = list;
        }
        self.apart.sort_unstable();
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
            // its crews and, a link each, which of them that player is in too: only to a player that reads them and
            // is in a crew (so no crews but its own go out)
            let mine = if p.marks && self.crewed > 0 { &p.crews[..crews_of(p).count()] } else { &[][..] };
            s.marks.clear();
            if !mine.is_empty() {
                s.marks.extend(links.iter().map(|l| match slots.get(l.slot as usize) {
                    Some(Some(q)) => mine.iter().enumerate().fold(0u8, |m, (c, h)| m | (q.crews.contains(h) as u8) << c),
                    _ => 0,
                }));
            }
            let n = F_DATA + if mine.is_empty() { wire::peers_len(links.len()) } else { wire::peers_crew_len(links.len(), mine.len()) };
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
        let was = self.cap;
        self.st.busy = js_round(busy * 100.0);
        let o = &self.o;
        if busy > o.busy_high && now - self.cap_at >= o.cap_down_ms {
            self.cap = o.min_degree.min(o.degree).max(self.cap * 3 / 4);
        } else if busy < o.busy_low && now - self.cap_at >= o.cap_up_ms {
            self.cap = o.degree.min(self.cap + 2);
        }
        if self.cap == was {
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
            "lobbyLinks": st.lobby_links,
            "crewLinks": st.crew_links,
            "crewSkips": st.crew_skips,
            "raceReports": st.race_reports,
            "cheatLines": st.cheat_lines,
            "watchesKept": self.kept.len(),
            "blocking": self.blocks.len(),
            "keptApart": self.apart.len(),
        })
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
fn prepare(slots: &[Option<Player>], list: &[u32], cap: usize, o: &RoamOptions, now: f64, s: &mut Scratch) {
    s.pos.clear();
    s.soon.clear();
    s.ahead.clear();
    s.view.clear();
    s.deg.clear();
    for (i, &slot) in list.iter().enumerate() {
        let p = player(slots, slot);
        let (at, v, status, heading, age) =
            p.map_or(([0.0; 3], [0.0; 3], 0, 0, 0.0), |p| (p.p.unwrap_or_default(), p.v, p.status, p.heading, (now - p.pos_at) / 1000.0));
        let (v, view) = if status == 2 { (v, o.view_m) } else { ([0.0; 3], 0.0) };
        let up = if matches!(status, 2 | 5) { 0.0 } else { o.away_m };
        let age = age.clamp(0.0, AGE_MAX_S);
        let on = |t: f64| [at[0] + v[0] * t, at[1] + v[1] * t, at[2] + v[2] * t + up];
        let soon = on(age + o.soon_ms / 1000.0);
        let a = f64::from(heading) * std::f64::consts::PI / 128.0;
        s.pos.push(at);
        s.soon.push(soon);
        s.ahead.push(on(age + o.look_ms / 1000.0));
        s.view.push([soon[0] + a.cos() * view, soon[1] + a.sin() * view, soon[2]]);
        // a player's links at most: its degree, under the cap
        s.deg.push(p.map_or(0, |p| p.degree.min(cap)));
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
/// "Nearest" here is the plain distance of the two places soon_ms on (`s.soon`, where the links weigh first: a car
/// moved on a second, a player not in free roam away_m up, so a crew-mate driving comes before one in a shop),
/// compared as its square: these pairs need no order but their own (the rest is the Node hub's order, bit for bit),
/// and four Math.hypot a pair (`apart`) would cost more than the rest of this.  The order decides only which of a
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
                let (a, b) = (&soon[i], &soon[j]);
                let (dx, dy, dz) = (a[0] - b[0], a[1] - b[1], a[2] - b[2]);
                let sq = dx * dx + dy * dy + dz * dz;
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
/// race lobbies' links and the crews' come first; returns how many of each were made.
fn link_every_pair(slots: &[Option<Player>], list: &[u32], w: &Weigh, s: &mut Scratch) -> (u64, u64) {
    let m = list.len();
    let Scratch {
        soon,
        ahead,
        view,
        deg,
        mark,
        since,
        pairs,
        order,
        sorting,
        local: at,
        fresh,
        lpairs,
        cpairs,
        ccount,
        npre,
        bstart,
        bnext,
        ..
    } = s;
    // the race lobbies' links first, then the crews'
    let linked = |fresh: &[Vec<Link>], a: u32, b: u32| fresh[a as usize].iter().any(|l| l.slot == b);
    let mut made = 0;
    for &(a, b) in lpairs.iter() {
        let (a, b) = (a as usize, b as usize);
        let (sa, sb) = (list[a], list[b]);
        if fresh[sa as usize].len() < deg[a]
            && fresh[sb as usize].len() < deg[b]
            && !linked(fresh, sa, sb)
            && !kept_apart(bstart, bnext, a, b)
        {
            join(fresh, sa, sb, since_of(slots, sa, sb, w.now));
            made += 1;
        }
    }
    let mut crew = 0;
    if !cpairs.is_empty() {
        ccount.clear();
        ccount.resize(m, 0);
        for &(_, a, b) in cpairs.iter() {
            let (a, b) = (a as usize, b as usize);
            let (sa, sb) = (list[a], list[b]);
            if ccount[a] < CREW_LINKS
                && ccount[b] < CREW_LINKS
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
    let first = made + crew > 0;
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
                apart(soon, ahead, view, i, j)
            } else if w.now - since[j] < w.hold_ms {
                -1.0
            } else {
                apart(soon, ahead, view, i, j) * w.keep
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
    (made, crew)
}

/// whether `places` has `j` (every one compared, without a branch: there are at most a degree of them, and this
/// vectorizes)
fn has_place(places: &[u32], j: u32) -> bool {
    places.iter().fold(false, |found, &x| found | (x == j))
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
/// into the tick: there it made the every-pair loop of the small rooms 1 % slower.)  The race lobbies' links and the
/// crews' come first, as in `link_every_pair`; returns how many of each were made.
#[inline(never)]
fn link_many(slots: &[Option<Player>], list: &[u32], w: &Weigh, s: &mut Scratch) -> (u64, u64) {
    let m = list.len();
    let Scratch {
        soon,
        ahead,
        view,
        deg,
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
        ostart,
        fcount,
        flinks,
        lpairs,
        cpairs,
        ccount,
        bstart,
        bnext,
        ..
    } = s;
    let cand = |d: f64, i: usize, j: usize, nearest: bool| Cand::new(d, i as u32, j as u32, nearest);
    grid.build(soon);
    grid_a.build(ahead);
    heap.clear();
    near.clear();
    near_a.clear();
    near_v.clear();
    olds.clear();
    olinks.clear();
    ostart.clear();
    for (i, &slot) in list.iter().enumerate() {
        ostart.push(olinks.len() as u32);
        if let Some(a) = player(slots, slot) {
            for l in &a.links {
                let Some(j) = local(at, l.slot) else { continue };
                olinks.push(j as u32);
                // each link once: by its lower end, or by the only end that lists it (j's links are in already)
                if i < j || !has_place(&olinks[ostart[j] as usize..ostart[j + 1] as usize], i as u32) {
                    // a link's place in the order: younger than `hold_ms` first, older at `keep` its distance
                    let d = if w.now - l.since < w.hold_ms { -1.0 } else { apart(soon, ahead, view, i, j) * w.keep };
                    olds.push(cand(d, i, j, false));
                }
            }
        }
        let mut n = Near::new(grid, soon, i as u32, found.pop().unwrap_or_default());
        if let Some((d, j)) = n.next(grid, soon) {
            heap.push(Cand::by(d, i as u32, j, WALK_SOON));
        }
        near.push(n);
        let mut n = Near::new(grid_a, ahead, i as u32, found.pop().unwrap_or_default());
        if let Some((d, j)) = n.next(grid_a, ahead) {
            heap.push(Cand::by(d, i as u32, j, WALK_AHEAD));
        }
        near_a.push(n);
        let mut n = Near::from(grid, view[i], i as u32, found.pop().unwrap_or_default());
        if view[i] != soon[i] {
            if let Some((d, j)) = n.next(grid, soon) {
                heap.push(Cand::by(d, i as u32, j, WALK_VIEW));
            }
        }
        near_v.push(n);
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
    // the race lobbies' links first, then the crews'
    let mut made = 0;
    for &(a, b) in lpairs.iter() {
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
            made += 1;
        }
    }
    let mut crew = 0;
    if !cpairs.is_empty() {
        ccount.clear();
        ccount.resize(m, 0);
        for &(_, a, b) in cpairs.iter() {
            let (i, j) = (a as usize, b as usize);
            if ccount[i] < CREW_LINKS
                && ccount[j] < CREW_LINKS
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
        if c.nearest() {
            // i's next nearest one of that walk, while it has room
            if room(fcount, i) {
                let next = match c.walk() {
                    WALK_AHEAD => near_a[i].next(grid_a, ahead),
                    WALK_VIEW => near_v[i].next(grid, soon),
                    _ => near[i].next(grid, soon),
                };
                if let Some((d, k)) = next {
                    heap.push(Cand::by(d, i as u32, k, c.walk()));
                }
            }
            // a link of the last tick: in the order at its own place
            if old(i, j) {
                continue;
            }
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
                player(slots, list[i]).and_then(|a| a.links.iter().find(|l| l.slot == list[j])).map_or(w.now, |l| l.since)
            };
            join(fresh, list[i], list[j], since);
            for (a, b) in [(i, j), (j, i)] {
                flinks[a * stride + fcount[a] as usize] = b as u32;
                fcount[a] += 1;
            }
        }
    }
    for n in near.drain(..).chain(near_a.drain(..)).chain(near_v.drain(..)) {
        found.push(n.into_found());
    }
    (made, crew)
}

/// whether one of the two players' browsers blocks the other (`Hub::blocks`)
fn blocked(blocks: &HashMap<u32, Block>, a: &Player, b: &Player) -> bool {
    blocks.get(&a.vip).is_some_and(|k| k.has(b)) || blocks.get(&b.vip).is_some_and(|k| k.has(a))
}
