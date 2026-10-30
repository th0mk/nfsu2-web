//! The free-roam hub.  Its links are the ones the Node server's hub made before it: the same pairs in the same order,
//! the grid and heap of a large room included.
//!
//! The hub is the server's host on the switch at 10.77.0.253 (runtime/web/net_proto.h NET_ROAM_HUB, an address the
//! switch never hands out) that tells each free-roam player which players are near it.  Without it every player
//! talks to every other one, in groups of at most 8 (the first to meet); with it a server takes many more players:
//! each player's peers are the hub's list of the players nearest to it (runtime/roam_core.c, hub mode), and the
//! snapshots still go straight from player to player through the switch.
//!
//! - player -> hub: a HELLO every second (its id, room, status, name, and its crews), a SNAP twice a second (where
//!   its car is), a BYE when it stops.
//! - hub -> player: twice a second (`tick_ms`) a PEERS message: the players linked to it, { virtual address, port, id }, at most
//!   `degree` (32: roam_core's group of 33 with itself), or 7 for a player whose HELLO says core version 1 (its group
//!   was 8, and it drops a longer list); an empty one says the hub is there.  To a player in a crew whose HELLO says
//!   it reads them (core 8), the list ends in crew marks: its own crews, then which of them each listed player is in.
//! - player -> hub -> player: when its world map opens, and each second while it is open, a MAPREQ; the hub answers
//!   with MAP datagrams (at most nine of 230): the other players of its room in free roam, past its links, nearest
//!   first, map_max (2048) of them at most, or as many as the MAPREQ asks for (512 from a page before 2048), each its
//!   place (x, y in whole metres) and heading (a byte), which the game marks on the map.
//!
//! The links, in each room: the pairs of players with a position younger than `stale_ms` (60 s: one in a shop or a
//! race keeps its links for a while), nearest first, each player at most its degree of them.  "Nearest" looks ahead,
//! for the cars a player will see: a pair counts at the nearest of
//! - its distance once both cars went on `soon_ms` (1 s) at the speeds their last SNAPs gave (from where those SNAPs
//!   had them, moved on by their age), so a car a player drives away from gives way at once,
//! - its distance once both went on `look_ms` (4 s), so a car a player heads for is linked (and its car loaded) before
//!   it is near,
//! - and the distance from either one's view point, `view_m` (150 m) ahead of where its car points (from its place
//!   soon_ms on), to the other one soon_ms on: the camera looks that way, so a car in front, parked or driving, shows
//!   from farther away than one beside or behind it.
//!
//! Only a car in free roam moves on and has a view point; a player not in free roam (a shop, a race, the menus,
//! loading) counts `away_m` (100 m) up, so its links go to the cars shown first.  Parked players in free roam count by
//! distance and view point.  A simulation of 1400 players (tests/popin.rs, 2026-10-23) against the links of before
//! (now and 3 s ahead, a tick a second): of the cars within 250 m in front of a driver 84 % shown instead of 63 %,
//! a car showing within 100 m in front 0.30 times a minute instead of 0.63, a car left behind gone at 300 m instead of
//! 390; for 600 players in 800 m square, 20 such pop-ins a minute instead of 66.  Both ends of a link list each other
//! (roam_core takes a player only when both hear each other).  A link stays at least `hold_ms` (5 s), and
//! after that counts as `keep` (half) its distance: a new link costs a handshake, the car's record and maybe a car
//! swapped on the screen, so it has to be much nearer to take the place of one (62 simulated players circling within
//! 240 m changed links 28 times a second with 20 % instead).  The game shows a car for each of a player's links
//! (runtime/roam_game.c).
//!
//! A race lobby's links come before all of these (its messages go between linked players only, roam_game.c mp_*):
//! from its host's advert (to the hub every second, with its players), the host is linked with each of its players,
//! with each player asking to join it (a copy of its JOIN to the hub, for 6 s), and with the 24 nearest players in
//! free roam within 20 m of its start, while both ends have room.  By distance alone a host at a busy start had its
//! links full of nearer players, the ones who went into the career race there among them: the link to a player who
//! stopped there to join came and went, and its Enter went nowhere.
//!
//! Then the crews' (a word each, up to 4 a player, set on the start page; its HELLO names them by roam_core's room
//! hash): two players of one room who share a crew are linked before the rest, nearest first, while both ends have
//! room, at most 16 such links a player, so crew-mates always see each other and still see the cars around them.  By
//! distance alone a friend 300 m off in a crowd was never linked (32 nearer players were, 8 under the busy cap): his
//! car, name and map arrow never showed.  A crew of more than 64 players in one room gets no links first (its pairs
//! grow as the square of its players): they link by distance as everyone does.  /players counts each crew's players
//! for the start page (`in_crews()`).  A player learns which of its peers share a crew from the hub's marks on its
//! list, never from the peers: a crew's hash in a stranger's hands is all it takes to be linked with that player
//! anywhere on the map, and a short crew name is quickly found from its hash (a core before 8 still sends its crews to
//! the peers that read them, and colours a crew-mate of core 8 no more).
//!
//! A busy server gives every player fewer links: each link is two players' snapshots to each other, 20 a second each
//! way (fewer for far ones), so a crowd costs its players times their links, all through the switch; a switch that
//! cannot keep up delays every snapshot, and the cars stutter for everyone (nfsu2.xyz at 855 players, 2026-10-20:
//! 180-600 ms for a /players answer on Node's one thread).  While the server worked more than `busy_high` of the last
//! tick the cap on the links goes down by a quarter, at most every `cap_down_ms`, to `min_degree` at the least; once
//! it has room again (under `busy_low`) the cap goes back up by 2 every `cap_up_ms`.  The nearest links stay.  While
//! the cap is under `ask_cap` (24) free roam counts as busy (the server's /players roamBusy): the start page does not
//! ask its question (Free roam with others?) at a Play then, so fewer new players join a crowd that already gets fewer
//! cars; it asks at a later Play once the cap is back.  /players also says how many drive in free roam, or are paused
//! there (`in_roam()`, counted at each tick).
//!
//! The hub also watches how each player's car moves (watch.rs): a teleport, a car faster than any of the game's, or a
//! game whose clock runs fast (a speed hack) is a `[cheat]` line with the player's address and name, and who was
//! next to where its car came to.  And it judges each race lobby's race by its racers' results (results.rs): a win
//! no car makes is a `[cheat]` line too.  Lines only: the operator decides (`nfsu2-server ctl kick|ban`).
//!
//! A claimed name (nfsu2-claims) is its keyphrase's only: a player whose HELLO names one that another keyphrase
//! claimed (its page's hello to the switch named the keyphrase it plays with: `FrameCtx::owner`) gets no links, is on
//! nobody's map and counts nowhere, and the console says so once.  The page plays under the name with four digits
//! then (as the Online lobby gives it), so only a page that ignores that plays alone.  A player's own claim hears that
//! it plays (it holds 180 days from its last game).
//!
//! A page may block players by the name over their car (its hello to the switch: `Host::blocks`, `Hub::blocks`): the
//! hub links it with none of them, either way, nor with the host of a race lobby one of them is in, and leaves each
//! out of the other's world map and lobby list.  Names count as their Online form (any case), and a claimed name's
//! keyphrase stays blocked under its next name while the old one is still its own.  The console has a line for each
//! page that blocks (`[roam] <address> blocks "<name>", ...`), so the operator sees who many players block.
//!
//! What the hub keeps, in memory only: per player endpoint its virtual address, port, id, room hash, crews' hashes,
//! status, name and last position (and what its watch counted), and per browser the names it blocks; gone `gone_ms`
//! after its last message, when it says BYE, or when it leaves the switch.  Reading positions is what choosing by
//! distance costs: the switch itself never looks inside a frame.  At
//! most `max_per_vip` endpoints per virtual address (a browser has one), and `max_players` in all.
//!
//! - [`Hub`]: the hub's logic, single-threaded, with the clock, the busy measure and the sending given to it (the
//!   tests drive it by its `frame()`, `tick()` and `info()`)
//! - [`RoamHub`]: the hub on a thread of its own, a `nfsu2_util::Host` for the switch: frames are posted to it in
//!   order, it ticks every `tick_ms`, and sends its lists through the switch.  Its "busy" is the share of their time
//!   the runtime's workers, which run the switch, spent working (nfsu2_util::cpu::WorkersBusy): Node read its one
//!   thread's event-loop use.  (The process's CPU time over every core, which it was, never passed `busy_high` once
//!   the workers were half the cores: all of them busy was 50 %.)  Or, where it is higher, the share of a CPU quota
//!   the server shares used (nfsu2_util::cpu::QuotaBusy): the game's containers share nfsu2.slice's, and cloudflared
//!   pays for every snapshot too, so a slice near its limit gets fewer links before its players get them late.

mod hub;
mod jumps;
mod links;
mod results;
mod watch;
mod wire;

use std::collections::{HashMap, HashSet};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use bytes::Bytes;
use nfsu2_util::cpu::{Busy, QuotaBusy, WorkersBusy};
use nfsu2_util::{FrameCtx, Host, SwitchSend};
use tokio::sync::oneshot;

pub use hub::Hub;
pub use wire::{decode_roam, encode_peers, encode_peers_crews};

/// 10.77.0.253, runtime/web/net_proto.h NET_ROAM_HUB
pub const ROAM_HUB_VIP: u32 = 0x0A4D_00FD;

/// the most crews a player is in (roam_core.h RC_CREW_MAX), and so the most /players counts for one question
pub const CREWS: usize = wire::CREWS;
/// the games (roam_core.h RC_GAME_*): `RoamOptions::game`
pub const GAME_UG2: u8 = 0;
pub const GAME_MW: u8 = 1;

/// The hub's options.  exact_room: a room of at most this many players sorts every pair of them; a larger one takes
/// the pairs in the same order but only as far as it needs them: each player's nearest one after another (a grid)
/// until it has its links (every pair of 2000 players in one room took the Node hub 1.5-4.5 s a tick, 1000 players
/// 0.5 s).  A value that is not a positive number keeps its default.
#[derive(Clone, Debug)]
pub struct RoamOptions {
    /// the game whose players it takes (roam_core.h RC_GAME_*: 0 Underground 2, 1 Most Wanted): a HELLO of another
    /// game is refused, and one without a game is Underground 2's (PLAN D-18)
    pub game: u8,
    pub degree: usize,
    pub tick_ms: u64,
    pub stale_ms: f64,
    pub gone_ms: f64,
    pub keep: f64,
    pub hold_ms: f64,
    pub max_per_vip: usize,
    pub max_players: usize,
    pub exact_room: usize,
    pub min_degree: usize,
    pub busy_high: f64,
    pub busy_low: f64,
    pub cap_down_ms: f64,
    pub cap_up_ms: f64,
    /// under this cap free roam is busy (`crowded()`)
    pub ask_cap: usize,
    /// how far ahead the links look (ms): a pair counts at the nearer of its distance once both cars went on soon_ms
    /// and once they went on look_ms at their speeds, so a car a player heads for is linked before it is near, and one
    /// it drives away from gives way at once
    pub soon_ms: f64,
    pub look_ms: f64,
    /// a player not in free roam (a shop, a race, the menus) counts this far up (m): its links go to the cars shown
    pub away_m: f64,
    /// a player's view point, this far ahead of where its car points (m): a car in front counts from there too, so it
    /// is linked from farther away than one beside or behind
    pub view_m: f64,
    /// the world map (MAPREQ): at most this many players a answer, one answer a browser every map_gap_ms at most, of
    /// the players in free roam (or paused there) with a position younger than map_fresh_ms
    pub map_max: usize,
    pub map_gap_ms: f64,
    pub map_fresh_ms: f64,
}

impl Default for RoamOptions {
    fn default() -> Self {
        RoamOptions {
            game: 0,
            degree: 32,
            tick_ms: 500,
            stale_ms: 60_000.0,
            gone_ms: 6_000.0,
            keep: 0.5,
            hold_ms: 5_000.0,
            max_per_vip: 2,
            max_players: 6144,
            exact_room: 256,
            min_degree: 8,
            busy_high: 0.85,
            busy_low: 0.65,
            cap_down_ms: 5_000.0,
            cap_up_ms: 15_000.0,
            ask_cap: 24,
            soon_ms: 1_000.0,
            look_ms: 4_000.0,
            away_m: 100.0,
            view_m: 150.0,
            map_max: 2048,
            map_gap_ms: 1_000.0,
            map_fresh_ms: 5_000.0,
        }
    }
}

/// The fields the hub reads of one free-roam datagram (`decode_roam`); a HELLO's crews are the ones it names (with the
/// crew flag), in its order
#[derive(Clone, Debug, PartialEq)]
pub enum RoamMsg {
    Hello { sender: u32, core: u16, status: u8, room: u32, name: String, crews: Vec<u32>, game: u8 },
    Snap { sender: u32, status: u8, p: [f64; 3] },
    Bye { sender: u32 },
    Other { kind: u8, sender: u32 },
}

/// `Math.hypot(x, y, z)` as V8 computes it: the hub's distances (public for tests/hypot.rs, which checks it against
/// node's own)
pub fn hypot(x: f64, y: f64, z: f64) -> f64 {
    links::hypot3(x, y, z)
}

/// one entry of a PEERS list
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PeerEntry {
    pub vip: u32,
    pub port: u16,
    pub id: u32,
}

/// What the switch posts to the hub's thread, in order.
enum Cmd {
    /// a frame from a browser, and the keyphrase its page plays with
    Frame(u32, Bytes, Option<Arc<str>>),
    Gone(u32),
    Attach(SwitchSend),
    Info(oneshot::Sender<serde_json::Value>),
    Vips(oneshot::Sender<HashSet<u32>>),
    Blocks(u32, Vec<String>),
    Stop,
}

/// Frames waiting for the hub's thread at most: it handles one in well under a microsecond and 2048 players send
/// 6144 a second, so what waits is what came during a tick (15 ms with 2000 players in one room: 100 frames), and
/// this many (1.3 s of them) means the thread is stuck; the frames past it are left out as a busy network leaves out
/// datagrams, so that a stuck hub cannot take the server's memory (Node's hub could not fall behind: it ran inside
/// the switch's own calls).  A frame waiting is a slice of the 16 KiB block its connection's reader read it into,
/// and keeps that block: 128 MiB at most (65,536 of them were 1 GiB).
const MAX_PENDING: usize = 1 << 13;

/// The hub on a thread of its own.
pub struct RoamHub {
    tx: mpsc::Sender<Cmd>,
    pending: Arc<AtomicUsize>,
    /// the hub's `crowded()`, `in_roam()`, `in_rooms()` and `crew_counts()` after its last tick, for /players (which
    /// must not wait for the hub's thread)
    crowded: Arc<AtomicBool>,
    in_roam: Arc<AtomicUsize>,
    in_rooms: Arc<Mutex<HashMap<u32, usize>>>,
    in_crews: Arc<Mutex<Vec<(u32, u32)>>>,
}

impl RoamHub {
    /// starts its thread; `log`: a console line (the cap's changes).  Called within the server's runtime, its busy
    /// measure is the runtime's workers' (the switch's) or a CPU quota's, the higher; outside one (tests), the
    /// process's over every core.
    pub fn spawn(o: RoamOptions, log: Arc<dyn Fn(&str) + Send + Sync>) -> Arc<RoamHub> {
        RoamHub::spawn_with(o, log, None)
    }

    /// `spawn`, with the claimed names (nfsu2-claims): a player under a name another keyphrase claimed plays alone
    pub fn spawn_with(o: RoamOptions, log: Arc<dyn Fn(&str) + Send + Sync>, claims: Option<Arc<nfsu2_claims::Claims>>) -> Arc<RoamHub> {
        let (tx, rx) = mpsc::channel();
        let pending = Arc::new(AtomicUsize::new(0));
        let busy: Box<dyn FnMut() -> f64 + Send> = match tokio::runtime::Handle::try_current() {
            Ok(rt) => {
                let mut b = WorkersBusy::new(&rt);
                let mut q = QuotaBusy::find();
                Box::new(move || b.sample().max(q.as_mut().map_or(0.0, QuotaBusy::sample)))
            }
            Err(_) => {
                let mut b = Busy::new(std::thread::available_parallelism().map_or(1, |n| n.get()));
                Box::new(move || b.sample())
            }
        };
        let waiting = pending.clone();
        // random and never 0 (roam_core.c's decoder, as `wire::decode`, drops a datagram from sender 0)
        let id = nfsu2_util::rand::int(1, 0xFFFF_FFFF) as u32;
        let mut hub = Hub::new(o, Box::new(|| nfsu2_util::now_ms() as f64), busy, Box::new(move |l| log(l)), Box::new(|_, _| false), id);
        hub.set_claims(claims);
        // (made here, not on its thread: until its first tick crowded() is what the hub begins with)
        let crowded = Arc::new(AtomicBool::new(hub.crowded()));
        let in_roam = Arc::new(AtomicUsize::new(0));
        let in_rooms = Arc::new(Mutex::new(HashMap::new()));
        let in_crews = Arc::new(Mutex::new(Vec::new()));
        let (busy_now, roam_now, rooms_now, crews_now) = (crowded.clone(), in_roam.clone(), in_rooms.clone(), in_crews.clone());
        std::thread::Builder::new()
            .name("roam-hub".into())
            .spawn(move || run(hub, rx, &waiting, &busy_now, &roam_now, &rooms_now, &crews_now))
            .expect("a thread for the free-roam hub");
        Arc::new(RoamHub { tx, pending, crowded, in_roam, in_rooms, in_crews })
    }

    /// the hub's `crowded()`: free roam is busy, the cap is under `ask_cap` (as of the hub's last tick)
    pub fn crowded(&self) -> bool {
        self.crowded.load(Ordering::Relaxed)
    }

    /// the hub's `in_roam()`: how many browsers drive in free roam, or are paused there (as of the hub's last tick)
    pub fn in_roam(&self) -> usize {
        self.in_roam.load(Ordering::Relaxed)
    }

    /// the hub's `in_rooms()` of one room: how many browsers of the room (roam_core's rc_room_hash, not 0) drive in
    /// free roam, or are paused there (as of the hub's last tick)
    pub fn in_room(&self, room: u32) -> usize {
        self.in_rooms.lock().map_or(0, |m| m.get(&room).copied().unwrap_or(0))
    }

    /// the hub's `in_crews()`: how many browsers drive in free roam, or are paused there, in each of these crews
    /// (roam_core's rc_room_hash of a crew's word; any room), in their order (as of the hub's last tick)
    pub fn in_crews(&self, crews: &[u32]) -> Vec<usize> {
        let counts = self.in_crews.lock();
        crews.iter().map(|&c| counts.as_ref().map_or(0, |t| hub::crew_count(t, c))).collect()
    }

    /// the switch's way to its browsers
    pub fn attach_switch(&self, send: SwitchSend) {
        let _ = self.tx.send(Cmd::Attach(send));
    }
}

/// a panic in the hub (a bug) leaves the hub's thread running: the panic hook has printed it, the next frame or
/// tick goes on (Node would have died of the exception, and every player with it)
fn guard(f: impl FnOnce()) {
    let _ = catch_unwind(AssertUnwindSafe(f));
}

/// The hub's thread: what the switch posts, in order, and a tick every tick_ms (the next one a tick_ms after this
/// one began).  It ends once the RoamHub is gone.
fn run(
    mut hub: Hub,
    rx: mpsc::Receiver<Cmd>,
    pending: &AtomicUsize,
    crowded: &AtomicBool,
    in_roam: &AtomicUsize,
    in_rooms: &Mutex<HashMap<u32, usize>>,
    in_crews: &Mutex<Vec<(u32, u32)>>,
) {
    let every = Duration::from_millis(hub.options().tick_ms);
    let mut next = Some(Instant::now() + every);
    // the crews' counts made at a tick, then swapped with /players' (the two tables are kept: no allocation a tick)
    let mut crews = Vec::new();
    loop {
        let cmd = match next {
            Some(at) => {
                let now = Instant::now();
                if now >= at {
                    guard(|| hub.tick());
                    crowded.store(hub.crowded(), Ordering::Relaxed);
                    in_roam.store(hub.in_roam(), Ordering::Relaxed);
                    let rooms = hub.in_rooms();
                    if let Ok(mut m) = in_rooms.lock() {
                        *m = rooms;
                    }
                    hub.crew_counts(&mut crews);
                    if let Ok(mut m) = in_crews.lock() {
                        std::mem::swap(&mut *m, &mut crews);
                    }
                    next = Some(now + every);
                    continue;
                }
                match rx.recv_timeout(at - now) {
                    Ok(c) => c,
                    Err(mpsc::RecvTimeoutError::Timeout) => continue,
                    Err(mpsc::RecvTimeoutError::Disconnected) => return,
                }
            }
            None => match rx.recv() {
                Ok(c) => c,
                Err(_) => return,
            },
        };
        match cmd {
            Cmd::Frame(from, data, owner) => {
                pending.fetch_sub(1, Ordering::Relaxed);
                guard(|| hub.frame_from(from, &data, owner.as_ref()));
            }
            Cmd::Gone(vip) => guard(|| hub.peer_gone(vip)),
            Cmd::Attach(send) => hub.attach_switch(Box::new(move |vip, f| send(vip, f, None).sent())),
            Cmd::Info(tx) => guard(|| {
                let _ = tx.send(hub.info());
            }),
            Cmd::Vips(tx) => guard(|| {
                let _ = tx.send(hub.vips());
            }),
            Cmd::Blocks(vip, names) => guard(|| hub.blocks(vip, names)),
            Cmd::Stop => {
                // no more ticks, so no more lists: the switch's send goes too (it holds the switch, which holds
                // this hub)
                next = None;
                hub.attach_switch(Box::new(|_, _| false));
            }
        }
    }
}

impl Host for RoamHub {
    fn vip(&self) -> u32 {
        ROAM_HUB_VIP
    }
    fn name(&self) -> &str {
        "free-roam hub"
    }
    fn listed(&self) -> bool {
        false
    }
    fn frame(&self, from: u32, data: Bytes, ctx: FrameCtx) {
        // the hub's lists are charged to nobody; the owner is for the claimed names
        let owner = ctx.owner;
        if self.pending.fetch_add(1, Ordering::Relaxed) >= MAX_PENDING {
            self.pending.fetch_sub(1, Ordering::Relaxed);
            return;
        }
        if self.tx.send(Cmd::Frame(from, data, owner)).is_err() {
            self.pending.fetch_sub(1, Ordering::Relaxed);
        }
    }
    fn peer_gone(&self, vip: u32) {
        let _ = self.tx.send(Cmd::Gone(vip));
    }
    fn info(&self) -> oneshot::Receiver<serde_json::Value> {
        let (tx, rx) = oneshot::channel();
        let _ = self.tx.send(Cmd::Info(tx));
        rx
    }
    fn vips(&self) -> oneshot::Receiver<HashSet<u32>> {
        let (tx, rx) = oneshot::channel();
        let _ = self.tx.send(Cmd::Vips(tx));
        rx
    }
    fn blocks(&self, vip: u32, names: Vec<String>) {
        let _ = self.tx.send(Cmd::Blocks(vip, names));
    }
    fn stop(&self) {
        let _ = self.tx.send(Cmd::Stop);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // a hub whose thread is stuck holds MAX_PENDING frames and leaves out the rest; each may keep
    // its reader's block of 16 KiB alive, 128 MiB at most
    #[test]
    fn a_stuck_hub_holds_few_frames() {
        let (tx, rx) = mpsc::channel();
        let hub = RoamHub {
            tx,
            pending: Arc::new(AtomicUsize::new(0)),
            crowded: Arc::new(AtomicBool::new(false)),
            in_roam: Arc::new(AtomicUsize::new(0)),
            in_rooms: Arc::new(Mutex::new(HashMap::new())),
            in_crews: Arc::new(Mutex::new(Vec::new())),
        };
        let ctx = FrameCtx { group: Arc::from("client"), site: None, cause: None, slot: None, owner: None };
        for _ in 0..100_000 {
            hub.frame(0x0A4D_0005, Bytes::from_static(b"a frame"), ctx.clone());
        }
        let held = rx.try_iter().filter(|c| matches!(c, Cmd::Frame(..))).count();
        assert_eq!(held, MAX_PENDING);
        assert!(held * (16 << 10) <= 128 << 20, "{held} frames, each a reader's block of 16 KiB");
    }

    // /players reads the hub's crowded() without its thread: as of the hub's last tick, and before the first one as
    // the hub begins (a hub of fewer links than `ask_cap` is busy from the start)
    #[test]
    fn what_players_reads_is_the_hubs_crowded() {
        let few = RoamHub::spawn(RoamOptions { degree: 16, ..RoamOptions::default() }, Arc::new(|_: &str| {}));
        assert!(few.crowded(), "16 links a player, under askCap 24: busy before any tick");
        let all = RoamHub::spawn(RoamOptions::default(), Arc::new(|_: &str| {}));
        assert!(!all.crowded(), "32 links a player");
        // a busy server: the cap down a quarter each tick (32, 24, 18), busy once it is under 24, and back once the
        // cap is (the thread's own loop, its ticks every 5 ms)
        let busy = Arc::new(std::sync::atomic::AtomicBool::new(true));
        let b = busy.clone();
        let o = RoamOptions { tick_ms: 5, cap_down_ms: 1.0, cap_up_ms: 1.0, ..RoamOptions::default() };
        let hub = Hub::new(
            o,
            Box::new(|| nfsu2_util::now_ms() as f64),
            Box::new(move || if b.load(Ordering::Relaxed) { 0.95 } else { 0.1 }),
            Box::new(|_| {}),
            Box::new(|_, _| false),
            1,
        );
        let (tx, rx) = mpsc::channel();
        let (pending, crowded) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicBool::new(false)));
        let c = crowded.clone();
        let t = std::thread::spawn(move || {
            run(hub, rx, &pending, &c, &AtomicUsize::new(0), &Mutex::new(HashMap::new()), &Mutex::new(Vec::new()))
        });
        let until = |want: bool| {
            let t0 = Instant::now();
            while crowded.load(Ordering::Relaxed) != want {
                assert!(t0.elapsed() < Duration::from_secs(5), "crowded {want}: not in 5 s");
                std::thread::sleep(Duration::from_millis(1));
            }
        };
        until(true);
        busy.store(false, Ordering::Relaxed);
        until(false);
        drop(tx);
        t.join().unwrap();
    }
}
