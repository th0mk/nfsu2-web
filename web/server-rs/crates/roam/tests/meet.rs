//! A car meet: free roam's links from the hub and the cars the games show for them, simulated, so that the hub's
//! options (and the games' way with their slots) can be compared where many players stand together and more drive
//! by.  Not a test: run it in release,
//!   web/server-rs/cargo.sh roam test --release -p nfsu2-roam --test meet -- --ignored --nocapture
//!
//! The players: about 90 at the meet, each one now parked at a spot of it (8 to 60 m from its middle, pointing any
//! way) for 1.5 to 6 minutes, now driving by on straight lines through and past it (up to 200 m off its middle, 15 to
//! 35 m/s), and now driving in to a spot or off from one; at a time about 50 stand and 40 drive.  Around them a city
//! of 300 players (3 km a side: some driving, some parked, some in a shop), and 6 who jump: 40 s far off, then 40 s
//! at a spot of the meet.  Each sends the hub a SNAP twice a second, each game asks for 64 links (core version 14),
//! and each game is a model of roam_game.c's 64 slots (`assign`): a peer listed is shown after the handshake and its
//! car loaded, the car loader serving one load after another, two queued at most; a slot of a peer gone is parked
//! with its look, for that peer again or another's look.
//!
//! Measured: for the players parked at the meet (20 s and more), how many cars show, how many of their 64 nearest
//! (by plain distance) show, how many cars a minute they load, and how many go while still within 60 m (a car that
//! vanishes in front of them); for the ones who jump in, how long until 90 % of what can show shows.

mod kit;

use std::collections::HashMap;

use kit::*;
use nfsu2_roam::RoamOptions;

struct Lcg(u64);

impl Lcg {
    fn unit(&mut self) -> f64 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        (self.0 >> 11) as f64 / (1u64 << 53) as f64
    }
    fn range(&mut self, a: f64, b: f64) -> f64 {
        a + (b - a) * self.unit()
    }
}

/// the simulation's step (s)
const STEP: f64 = 0.1;
/// the game's slots (roam_game.c NSLOT) and the links it asks for
const SLOTS: usize = 64;
/// from a list to a peer: the list on its way and HELLOs both ways through the switch, and the car's record (s)
const HANDSHAKE: f64 = 0.5;
/// roam_core.c T_HUB_GRACE (s)
const GRACE: f64 = 2.5;
/// roam_game.c LOADS_AHEAD
const LOADS_AHEAD: usize = 2;
/// "near" for a car that goes: one that goes this near is one that vanished in front of the player
const NEAR_M: f64 = 60.0;
/// how many players: at the meet, in the city round it (side m a side), and the ones who jump in
#[derive(Clone, Copy)]
struct Scene {
    name: &'static str,
    meet: usize,
    city: usize,
    side: f64,
    jump: usize,
    /// the players' links as live (most at a computer's 32, phones 16, tablets 24, some 48 or 64); the ones measured
    /// (every 4th of the meet's, and the jumpers) at 64 all the same
    mix: bool,
    /// the links of the ones measured (64; with mix: 16 a phone's, 32 a computer's default)
    watch_links: u8,
    /// a meet as players hold one: the parked ones now and then do a donut or move to another spot, go into a shop or
    /// pause for a while, and some drivers circle the meet (FIDGET_S, LOOPERS)
    real: bool,
}
/// in a real meet (`Scene::real`): a parked player's next donut, move or stop comes this often (s, on average), and
/// this share of the drivers circle the meet instead of driving through
const FIDGET_S: f64 = 25.0;
const LOOPERS: f64 = 0.35;
const JUMP_N: usize = 6;

#[derive(Clone, Copy, PartialEq)]
enum Doing {
    /// at a spot of the meet until then
    Parked { until: f64 },
    /// along a leg: from a to b at speed s, from t0; then (park: at b)
    Leg { a: [f64; 2], b: [f64; 2], t0: f64, s: f64, park: bool },
    /// a city player: straight on, now and then a turn (popin.rs)
    City,
    /// a jumper far off until then
    Away { until: f64 },
    /// a parked player's donut or move (yaw rate w, speed sp) until then, then parked again until park
    Fidget { until: f64, w: f64, sp: f64, park: f64 },
    /// a parked player in a shop (status 4) or paused (5) until then, then parked again until park
    Stop { until: f64, park: f64 },
    /// circling the meet's middle at radius rr, angle a0 at t0, angular speed w (rad/s), until then (then a line)
    Loop { rr: f64, a0: f64, t0: f64, w: f64, until: f64 },
}

#[derive(Clone, Copy)]
struct Car {
    p: [f64; 3],
    v: [f64; 3],
    h: f64,
    turn: f64,
    status: u8,
    next_snap: f64,
    doing: Doing,
    /// parked since (for the measure)
    since: f64,
    /// a real meet's next donut, move or stop (`Scene::real`)
    next_fidget: f64,
}

impl Car {
    fn speed(&self) -> f64 {
        (self.v[0] * self.v[0] + self.v[1] * self.v[1]).sqrt()
    }
}

#[derive(Clone, Copy, PartialEq)]
enum SlotSt {
    Free,
    /// a car parked with this peer's look
    Parked(usize),
    /// loading for this peer until then
    Loading(usize, f64),
    Active(usize),
}

struct Peer {
    listed: f64,
    first: f64,
}

#[derive(Default)]
struct Game {
    peers: HashMap<usize, Peer>,
    slots: Vec<SlotSt>,
    /// the car loader busy until then
    loader_at: f64,
    /// a jump in: when, and whether the 90 % mark was reached
    jumped: Option<f64>,
    linked90: bool,
    /// since when each peer's car shows, and when each one went
    shown_at: HashMap<usize, f64>,
    gone_at: HashMap<usize, f64>,
}

/// how a game ranks its peers for the slots (roam_game.c PeerView.rank): the nearest of the distances `soon` and
/// `look` seconds on, both cars at their speeds, and from its view point `view` m ahead (soon) to the other car
#[derive(Clone, Copy)]
struct Rank {
    soon: f64,
    look: f64,
    view: f64,
}

impl Rank {
    fn of(&self, a: &Car, b: &Car) -> f64 {
        let (dx, dy) = (b.p[0] - a.p[0], b.p[1] - a.p[1]);
        let (ux, uy) = (b.v[0] - a.v[0], b.v[1] - a.v[1]);
        let at = |t: f64| ((dx + ux * t).powi(2) + (dy + uy * t).powi(2)).sqrt();
        let (sx, sy) = (dx + ux * self.soon - a.h.cos() * self.view, dy + uy * self.soon - a.h.sin() * self.view);
        let view = if a.status == 2 { (sx * sx + sy * sy).sqrt() } else { f64::INFINITY };
        at(self.soon).min(at(self.look)).min(view)
    }
}

struct Way {
    name: &'static str,
    o: RoamOptions,
    rank: Rank,
    /// the car loader's time for one car (s)
    load: f64,
    /// the games' core version (15: one-way links from the hub)
    core: u16,
}

#[derive(Default)]
struct Tally {
    samples: u64,
    shown: u64,
    nearest: u64,
    nearest_shown: u64,
    near60: u64,
    near60_shown: u64,
    loads: u64,
    gone_near: u64,
    observer_s: f64,
    jump_fill: Vec<f64>,
    jump_links: Vec<f64>,
    jump_miss: u64,
    drive_samples: u64,
    drive_near: u64,
    drive_near_shown: u64,
    links_made: u64,
    /// for the players parked at the meet: how long each car showed that went, and how many came back within 30 s
    lives: Vec<f64>,
    back30: u64,
    /// ...and of the ones that went within NEAR_M, how long each showed
    near_lives: Vec<f64>,
}

fn pct(v: &mut [f64], q: f64) -> f64 {
    if v.is_empty() {
        return f64::NAN;
    }
    v.sort_by(f64::total_cmp);
    v[((v.len() - 1) as f64 * q).round() as usize]
}

fn spot(r: &mut Lcg) -> [f64; 2] {
    // uniform over the ring 8..60 m
    let rr = (r.range(8.0 * 8.0, 60.0 * 60.0)).sqrt();
    let a = r.unit() * std::f64::consts::TAU;
    [rr * a.cos(), rr * a.sin()]
}

/// a line through or past the meet: up to 200 m off its middle, 600 m either side
fn line(r: &mut Lcg, t: f64) -> Doing {
    let a = r.unit() * std::f64::consts::TAU;
    let (d, n) = ([a.cos(), a.sin()], [-a.sin(), a.cos()]);
    let b = r.range(-200.0, 200.0);
    let from = [n[0] * b - d[0] * 600.0, n[1] * b - d[1] * 600.0];
    let to = [n[0] * b + d[0] * 600.0, n[1] * b + d[1] * 600.0];
    Doing::Leg { a: from, b: to, t0: t, s: r.range(15.0, 35.0), park: false }
}

#[allow(non_snake_case)]
fn run(sc: &Scene, way: &Way) {
    let (MEET_N, CITY_N, half) = (sc.meet, sc.city, sc.side / 2.0);
    let mut r = Lcg(7);
    let mut k = Kit::new(RoamOptions { max_players: 8192, ..way.o.clone() });
    let n = MEET_N + CITY_N + sc.jump;
    let vip = |i: usize| 0x0A4D_0100 + i as u32;
    let mut cars: Vec<Car> = Vec::with_capacity(n);
    for i in 0..n {
        let mut c = Car {
            p: [0.0; 3],
            v: [0.0; 3],
            h: r.unit() * std::f64::consts::TAU,
            turn: 0.0,
            status: 2,
            next_snap: r.unit() * 0.5,
            doing: Doing::City,
            since: 0.0,
            next_fidget: if sc.real && i < sc.meet { r.range(0.0, 2.0 * FIDGET_S) } else { f64::INFINITY },
        };
        if i < MEET_N {
            if r.unit() < 0.55 {
                let s = spot(&mut r);
                c.p = [s[0], s[1], 0.0];
                c.doing = Doing::Parked { until: r.range(10.0, 360.0) };
            } else {
                let t0 = -r.range(0.0, 40.0);
                c.doing = line(&mut r, t0);
            }
        } else if i < MEET_N + CITY_N {
            c.p = [r.range(-half, half), r.range(-half, half), 0.0];
            let kind = r.unit();
            if kind < 0.55 {
                let s = r.range(15.0, 50.0);
                c.v = [c.h.cos() * s, c.h.sin() * s, 0.0];
            } else if kind >= 0.9 {
                c.status = 4;
            }
        } else {
            c.doing = Doing::Away { until: r.range(0.0, 40.0) };
            c.p = [half + 500.0, half + 500.0 + i as f64 * 30.0, 0.0];
        }
        cars.push(c);
    }
    let watched = |i: usize| (i < MEET_N && (!sc.mix || i.is_multiple_of(4))) || i >= MEET_N + CITY_N;
    let mut links_of = vec![SLOTS; n];
    for i in 0..n {
        let links = if !sc.mix {
            SLOTS as u8
        } else if watched(i) {
            sc.watch_links
        } else {
            let u = r.unit();
            if u < 0.6 {
                32
            } else if u < 0.7 {
                24
            } else if u < 0.9 {
                16
            } else if u < 0.95 {
                48
            } else {
                64
            }
        };
        links_of[i] = links as usize;
        k.from(vip(i), &hello_with(1000 + i as u32, &HelloOpts { core: way.core, links, ..Default::default() }));
    }
    let mut games: Vec<Game> = (0..n).map(|i| Game { slots: vec![SlotSt::Free; links_of[i]], ..Default::default() }).collect();
    let mut last_list = vec![f64::NEG_INFINITY; n];
    let (warm, end) = (60.0, 420.0);
    // the players measured: parked at the meet (20 s and more, 0), and driving round it or through it (1)
    let mut tallies = [Tally::default(), Tally::default()];
    let observed = |me: usize, a: &Car, t: f64| -> Option<usize> {
        if !watched(me) || me >= MEET_N || t < warm {
            return None;
        }
        let rr = (a.p[0] * a.p[0] + a.p[1] * a.p[1]).sqrt();
        match a.doing {
            Doing::Parked { .. } | Doing::Fidget { .. } if t - a.since >= 20.0 && rr < 70.0 => Some(0),
            Doing::Loop { .. } => Some(1),
            Doing::Leg { .. } if rr < 150.0 => Some(1),
            _ => None,
        }
    };
    let (mut next_tick, mut t, mut step_no) = (0.0, 0.0, 0u64);
    while t < end {
        // the cars move
        for c in cars.iter_mut() {
            match c.doing {
                Doing::Parked { until } if t >= c.next_fidget && t < until => {
                    c.next_fidget = t - FIDGET_S * (1.0 - r.unit()).ln();
                    let u = r.unit();
                    c.doing = if u < 0.45 {
                        // a donut: 6 to 12 m/s round a circle of 4 to 8 m, 3 to 8 s
                        let sp = r.range(6.0, 12.0);
                        let w = sp / r.range(4.0, 8.0) * if r.unit() < 0.5 { -1.0 } else { 1.0 };
                        Doing::Fidget { until: t + r.range(3.0, 8.0), w, sp, park: until }
                    } else if u < 0.75 {
                        // to another spot near: 4 to 8 m/s for 2 to 6 s, turning a little
                        Doing::Fidget { until: t + r.range(2.0, 6.0), w: r.range(-0.4, 0.4), sp: r.range(4.0, 8.0), park: until }
                    } else {
                        // a shop or the pause menu, 10 to 60 s
                        c.status = if r.unit() < 0.5 { 4 } else { 5 };
                        Doing::Stop { until: t + r.range(10.0, 60.0), park: until }
                    };
                }
                Doing::Fidget { until, w, sp, park } => {
                    c.h += w * STEP;
                    c.v = [c.h.cos() * sp, c.h.sin() * sp, 0.0];
                    c.p[0] += c.v[0] * STEP;
                    c.p[1] += c.v[1] * STEP;
                    if t >= until {
                        c.v = [0.0; 3];
                        c.doing = Doing::Parked { until: park.max(t + 5.0) };
                    }
                }
                Doing::Stop { until, park } => {
                    c.v = [0.0; 3];
                    if t >= until {
                        c.status = 2;
                        c.doing = Doing::Parked { until: park.max(t + 5.0) };
                    }
                }
                Doing::Loop { rr, a0, t0, w, until } => {
                    let a = a0 + w * (t - t0);
                    c.p = [rr * a.cos(), rr * a.sin(), 0.0];
                    let sp = w * rr;
                    c.v = [-a.sin() * sp, a.cos() * sp, 0.0];
                    c.h = c.v[1].atan2(c.v[0]);
                    if t >= until {
                        c.doing = line(&mut r, t);
                    }
                }
                Doing::Parked { until } => {
                    c.v = [0.0; 3];
                    if t >= until {
                        // off, the way it points, to 600 m
                        let d = [c.h.cos(), c.h.sin()];
                        let to = [c.p[0] + d[0] * 600.0, c.p[1] + d[1] * 600.0];
                        c.doing = Doing::Leg { a: [c.p[0], c.p[1]], b: to, t0: t, s: r.range(15.0, 25.0), park: false };
                    }
                }
                Doing::Leg { a, b, t0, s, park } => {
                    let (dx, dy) = (b[0] - a[0], b[1] - a[1]);
                    let len = (dx * dx + dy * dy).sqrt();
                    let u = ((t - t0) * s).min(len);
                    let d = [dx / len, dy / len];
                    c.p = [a[0] + d[0] * u, a[1] + d[1] * u, 0.0];
                    c.v = [d[0] * s, d[1] * s, 0.0];
                    c.h = d[1].atan2(d[0]);
                    if u >= len {
                        if park {
                            c.v = [0.0; 3];
                            c.doing = Doing::Parked { until: t + r.range(90.0, 360.0) };
                            c.since = t;
                        } else if sc.real && r.unit() < LOOPERS {
                            // round the meet, 40 to 150 m out at 10 to 25 m/s, for 20 to 90 s
                            let rr = r.range(40.0, 150.0);
                            let w = r.range(10.0, 25.0) / rr * if r.unit() < 0.5 { -1.0 } else { 1.0 };
                            c.doing = Doing::Loop { rr, a0: r.unit() * std::f64::consts::TAU, t0: t, w, until: t + r.range(20.0, 90.0) };
                        } else if r.unit() < 0.4 {
                            // in to a spot, from 500 m
                            let sp = spot(&mut r);
                            let a = r.unit() * std::f64::consts::TAU;
                            let from = [sp[0] - a.cos() * 500.0, sp[1] - a.sin() * 500.0];
                            c.doing = Doing::Leg { a: from, b: sp, t0: t, s: r.range(12.0, 20.0), park: true };
                        } else {
                            c.doing = line(&mut r, t);
                        }
                    }
                }
                Doing::City => {
                    let moving = c.status == 2 && c.speed() > 1.0;
                    if moving && c.turn == 0.0 && r.unit() < STEP / 6.0 {
                        c.turn = (r.unit() - 0.5) * std::f64::consts::PI;
                    }
                    if c.turn != 0.0 {
                        let a = c.turn.clamp(-1.047 * STEP, 1.047 * STEP);
                        c.turn -= a;
                        if c.turn.abs() < 1e-9 {
                            c.turn = 0.0;
                        }
                        let (s, co) = (a.sin(), a.cos());
                        c.v = [c.v[0] * co - c.v[1] * s, c.v[0] * s + c.v[1] * co, 0.0];
                    }
                    for a in 0..2 {
                        c.p[a] += c.v[a] * STEP;
                        if c.p[a].abs() > half {
                            c.p[a] = c.p[a].signum() * half;
                            c.v[a] = -c.v[a];
                        }
                    }
                    if moving {
                        c.h = c.v[1].atan2(c.v[0]);
                    }
                }
                Doing::Away { until } => {
                    if t >= until {
                        let sp = spot(&mut r);
                        c.p = [sp[0], sp[1], 0.0];
                        c.h = r.unit() * std::f64::consts::TAU;
                        c.doing = Doing::Parked { until: t + 40.0 };
                        c.since = t;
                    }
                }
            }
        }
        // jumpers away again after their 40 s
        for i in MEET_N + CITY_N..n {
            if let Doing::Parked { until } = cars[i].doing
                && t >= until - STEP
            {
                cars[i].doing = Doing::Away { until: t + 40.0 };
                cars[i].p = [half + 500.0, half + 500.0 + i as f64 * 30.0, 0.0];
                cars[i].v = [0.0; 3];
            }
        }
        for (i, c) in cars.iter_mut().enumerate() {
            if t >= c.next_snap {
                c.next_snap += 0.5;
                k.from(vip(i), &snap_moving(1000 + i as u32, c.p, c.v, c.h, c.status));
            }
        }
        if t >= next_tick {
            next_tick += way.o.tick_ms as f64 / 1000.0;
            k.clear();
            k.hub.tick();
            let lists: Vec<(usize, Vec<usize>)> = k
                .lists_in()
                .into_iter()
                .map(|(key, list)| {
                    let me = (key.split(':').next().unwrap().parse::<u32>().unwrap() - vip(0)) as usize;
                    (me, list.iter().map(|e| (e.vip - vip(0)) as usize).collect())
                })
                .collect();
            for (me, list) in lists {
                last_list[me] = t;
                for q in list {
                    let g = &mut games[me];
                    let p = g.peers.entry(q).or_insert_with(|| {
                        if t >= warm && me < MEET_N {
                            tallies[0].links_made += 1;
                        }
                        Peer { listed: t, first: t }
                    });
                    p.listed = t;
                }
            }
        }
        // the jumpers' jumps in
        for i in MEET_N + CITY_N..n {
            let landed = matches!(cars[i].doing, Doing::Parked { .. }) && (t - cars[i].since).abs() < STEP / 2.0;
            if landed && t >= warm {
                games[i].jumped = Some(t);
            }
        }
        // each game (of the players watched: the meet's and the jumpers'; the city's games change nothing the hub does)
        for me in (0..MEET_N).chain(MEET_N + CITY_N..n) {
            let a = cars[me];
            let ll = last_list[me];
            let class = observed(me, &a, t);
            let observer = class.is_some();
            let tally = &mut tallies[class.unwrap_or(0)];
            let g = &mut games[me];
            // peers gone
            let mut gone = Vec::new();
            g.peers.retain(|&q, p| {
                let keep = t - p.listed <= GRACE;
                if !keep {
                    gone.push(q);
                }
                keep
            });
            let near = |q: usize| ((cars[q].p[0] - a.p[0]).powi(2) + (cars[q].p[1] - a.p[1]).powi(2)).sqrt() < NEAR_M;
            for sl in g.slots.iter_mut() {
                match *sl {
                    SlotSt::Active(q) | SlotSt::Loading(q, _) if gone.contains(&q) => {
                        let went_near = observer && matches!(*sl, SlotSt::Active(_)) && near(q) && cars[q].status == 2;
                        if went_near {
                            tally.gone_near += 1;
                        }
                        if let Some(t0) = g.shown_at.remove(&q)
                            && observer
                        {
                            tally.lives.push(t - t0);
                            if went_near {
                                tally.near_lives.push(t - t0);
                            }
                        }
                        g.gone_at.insert(q, t);
                        *sl = if matches!(*sl, SlotSt::Active(_)) { SlotSt::Parked(q) } else { SlotSt::Free };
                    }
                    SlotSt::Loading(q, done) if t >= done => {
                        *sl = SlotSt::Active(q);
                        g.shown_at.insert(q, t);
                    }
                    _ => {}
                }
            }
            // slots: the nearest listed peer without one, while the loader has room
            let has = |slots: &[SlotSt], q: usize| slots.iter().any(|s| matches!(*s, SlotSt::Active(x) | SlotSt::Loading(x, _) if x == q));
            let mut cands: Vec<(f64, usize)> = g
                .peers
                .iter()
                .filter(|(q, p)| cars[**q].status == 2 && t >= p.first + HANDSHAKE && p.listed == ll && !has(&g.slots, **q))
                .map(|(&q, _)| (way.rank.of(&a, &cars[q]), q))
                .collect();
            cands.sort_by(|x, y| x.0.total_cmp(&y.0).then(x.1.cmp(&y.1)));
            for (d, q) in cands {
                // 1. a parked car with this look
                if let Some(j) = g.slots.iter().position(|s| *s == SlotSt::Parked(q)) {
                    g.slots[j] = SlotSt::Active(q);
                    g.shown_at.insert(q, t);
                    if observer && g.gone_at.get(&q).is_some_and(|&w| t - w < 30.0) {
                        tally.back30 += 1;
                    }
                    continue;
                }
                let queued = g.slots.iter().filter(|s| matches!(s, SlotSt::Loading(..))).count();
                if queued >= LOADS_AHEAD {
                    break;
                }
                // 2. a free slot, 3. a parked one, 4.-6. one to give way
                let mut j =
                    g.slots.iter().position(|s| *s == SlotSt::Free).or_else(|| g.slots.iter().position(|s| matches!(s, SlotSt::Parked(_))));
                if j.is_none() {
                    let rank_of = |x: usize| way.rank.of(&a, &cars[x]);
                    let mut out: Option<(f64, usize)> = None;
                    for (jj, s) in g.slots.iter().enumerate() {
                        if let SlotSt::Active(x) = *s
                            && (cars[x].status != 2 || g.peers.get(&x).is_some_and(|p| p.listed != ll))
                        {
                            let dx = rank_of(x);
                            if out.is_none_or(|(f, _)| dx > f) {
                                out = Some((dx, jj));
                            }
                        }
                    }
                    if out.is_none() {
                        let mut far: Option<(f64, usize)> = None;
                        for (jj, s) in g.slots.iter().enumerate() {
                            if let SlotSt::Active(x) = *s {
                                let dx = rank_of(x);
                                if far.is_none_or(|(f, _)| dx > f) {
                                    far = Some((dx, jj));
                                }
                            }
                        }
                        out = far.filter(|&(f, _)| d < f * 0.6 && f - d > 50.0);
                    }
                    let Some((_, jj)) = out else { break };
                    if let SlotSt::Active(x) = g.slots[jj] {
                        let went_near = observer && near(x) && cars[x].status == 2;
                        if went_near {
                            tally.gone_near += 1;
                        }
                        if let Some(t0) = g.shown_at.remove(&x)
                            && observer
                        {
                            tally.lives.push(t - t0);
                            if went_near {
                                tally.near_lives.push(t - t0);
                            }
                        }
                        g.gone_at.insert(x, t);
                    }
                    j = Some(jj);
                }
                let j = j.unwrap();
                let done = g.loader_at.max(t) + way.load;
                g.loader_at = done;
                g.slots[j] = SlotSt::Loading(q, done);
                if observer {
                    tally.loads += 1;
                    if g.gone_at.get(&q).is_some_and(|&w| t - w < 30.0) {
                        tally.back30 += 1;
                    }
                }
            }
            if observer {
                tally.observer_s += STEP;
            }
        }
        // what the players see, every half second
        if t >= warm && step_no % 5 == 0 {
            for me in (0..MEET_N).chain(MEET_N + CITY_N..n) {
                let a = cars[me];
                let g = &games[me];
                let shown: Vec<usize> = g
                    .slots
                    .iter()
                    .filter_map(|s| if let SlotSt::Active(q) = *s { Some(q) } else { None })
                    .filter(|&q| cars[q].status == 2)
                    .collect();
                let dist = |q: usize| ((cars[q].p[0] - a.p[0]).powi(2) + (cars[q].p[1] - a.p[1]).powi(2)).sqrt();
                let mut others: Vec<(f64, usize)> =
                    (0..n).filter(|&q| q != me && cars[q].status == 2).map(|q| (dist(q), q)).filter(|e| e.0 < 300.0).collect();
                others.sort_by(|x, y| x.0.total_cmp(&y.0));
                let kk = others.len().min(links_of[me]);
                let kj = others.iter().take_while(|e| e.0 < 100.0).count().min(SLOTS);
                if let Some(t0) = g.jumped {
                    let tally = &mut tallies[0];
                    let ll = last_list[me];
                    let linked = others[..kj].iter().filter(|e| g.peers.get(&e.1).is_some_and(|p| p.listed == ll)).count();
                    if linked as f64 >= 0.9 * kj as f64 && !g.linked90 {
                        tally.jump_links.push(t - t0);
                        games[me].linked90 = true;
                    }
                    let got = others[..kj].iter().filter(|e| shown.contains(&e.1)).count();
                    if got as f64 >= 0.9 * kj as f64 {
                        tally.jump_fill.push(t - t0);
                        games[me].jumped = None;
                        games[me].linked90 = false;
                    } else if t - t0 > 35.0 {
                        tally.jump_miss += 1;
                        games[me].jumped = None;
                        games[me].linked90 = false;
                    }
                }
                let g = &games[me];
                let class = observed(me, &a, t);
                if let Some(c) = class {
                    let tally = &mut tallies[c];
                    tally.samples += 1;
                    tally.shown += shown.len() as u64;
                    tally.nearest += kk as u64;
                    tally.nearest_shown += others[..kk].iter().filter(|e| shown.contains(&e.1)).count() as u64;
                    for e in others.iter().take_while(|e| e.0 < NEAR_M) {
                        tally.near60 += 1;
                        tally.near60_shown += shown.contains(&e.1) as u64;
                    }
                }
                // a driver going by the meet: the cars within 100 m
                if watched(me) && me < MEET_N && matches!(a.doing, Doing::Leg { .. }) && (a.p[0] * a.p[0] + a.p[1] * a.p[1]).sqrt() < 150.0
                {
                    let tally = &mut tallies[0];
                    tally.drive_samples += 1;
                    for e in others.iter().take_while(|e| e.0 < 100.0) {
                        tally.drive_near += 1;
                        tally.drive_near_shown += shown.contains(&e.1) as u64;
                    }
                }
                let _ = g;
            }
        }
        k.step(STEP * 1000.0);
        t += STEP;
        step_no += 1;
    }
    let pc = |a: u64, b: u64| 100.0 * a as f64 / b.max(1) as f64;
    let jumps = {
        let t0 = &mut tallies[0];
        format!(
            "jump in: 90 % linked {:>4.1} s, shown {:>4.1} s (p90 {:>4.1}, {} never) | links {:.2}/s | driving by: <100 m {:>4.1} %",
            pct(&mut t0.jump_links, 0.5),
            pct(&mut t0.jump_fill.clone(), 0.5),
            pct(&mut t0.jump_fill, 0.9),
            t0.jump_miss,
            t0.links_made as f64 / (MEET_N as f64 * (end - warm)),
            pc(t0.drive_near_shown, t0.drive_near),
        )
    };
    for (c, tally) in tallies.iter_mut().enumerate() {
        let mins = tally.observer_s / 60.0;
        println!(
            "{:<6} {:<26} {}: shown {:>4.1}, nearest N {:>4.1} %, <60 m {:>4.1} %, loads {:>5.1}/min, gone <60 m {:>5.2}/min (shown {:>4.1} s, median), \
             all gone: shown <10 s {:>3.0} % (median {:>4.1} s), back within 30 s {:>4.1}/min{}",
            sc.name,
            way.name,
            if c == 0 { "parked " } else { "driving" },
            tally.shown as f64 / tally.samples.max(1) as f64,
            pc(tally.nearest_shown, tally.nearest),
            pc(tally.near60_shown, tally.near60),
            tally.loads as f64 / mins,
            tally.gone_near as f64 / mins,
            pct(&mut tally.near_lives, 0.5),
            pc(tally.lives.iter().filter(|&&x| x < 10.0).count() as u64, tally.lives.len() as u64),
            pct(&mut tally.lives.clone(), 0.5),
            tally.back30 as f64 / mins,
            if c == 1 { format!(" | {jumps} | the hub's near links gone, why: {}", k.hub.info()["nearGone"]) } else { String::new() },
        );
    }
}

#[test]
#[ignore]
fn meet() {
    let now = RoamOptions::default();
    let game = |o: &RoamOptions| Rank { soon: o.soon_ms / 1000.0, look: o.look_ms / 1000.0, view: o.view_m };
    let way = |name, o: RoamOptions, core| Way { name, rank: game(&o), o, load: 0.12, core };
    let only = std::env::var("MEET_ONLY").unwrap_or_default();
    // the hub of before (links both ways: a game of core 14) and one-way links (core 15)
    let before = RoamOptions { near_m: 1e-9, ..now.clone() };
    let ways = [
        way("links both ways (core 14)", now.clone(), 14),
        way("one-way (15, released)", before, 15),
        way("one-way, near first", now.clone(), 15),
        way("one-way, near first at any speed", RoamOptions { near_fast: 1e9, ..now.clone() }, 15),
    ];
    let scenes = [
        Scene { name: "meet", meet: 90, city: 300, side: 3000.0, jump: JUMP_N, mix: false, watch_links: 64, real: false },
        Scene { name: "busy", meet: 45, city: 3000, side: 4000.0, jump: JUMP_N, mix: false, watch_links: 64, real: false },
        Scene { name: "mixed", meet: 90, city: 3000, side: 4000.0, jump: JUMP_N, mix: true, watch_links: 64, real: false },
        Scene { name: "mix32", meet: 90, city: 3000, side: 4000.0, jump: JUMP_N, mix: true, watch_links: 32, real: false },
        Scene { name: "mix16", meet: 90, city: 3000, side: 4000.0, jump: JUMP_N, mix: true, watch_links: 16, real: false },
        Scene { name: "real", meet: 90, city: 3000, side: 4000.0, jump: JUMP_N, mix: true, watch_links: 64, real: true },
        Scene { name: "real32", meet: 90, city: 3000, side: 4000.0, jump: JUMP_N, mix: true, watch_links: 32, real: true },
        Scene { name: "real16", meet: 90, city: 3000, side: 4000.0, jump: JUMP_N, mix: true, watch_links: 16, real: true },
        // as dense as the player's meet of 2026-11-05 (60 and more within 60 m)
        Scene { name: "dense", meet: 160, city: 3000, side: 4000.0, jump: JUMP_N, mix: true, watch_links: 64, real: true },
        Scene { name: "dense32", meet: 160, city: 3000, side: 4000.0, jump: JUMP_N, mix: true, watch_links: 32, real: true },
    ];
    let scene = std::env::var("MEET_SCENE").unwrap_or_default();
    for sc in scenes.iter().filter(|s| scene.is_empty() || s.name == scene) {
        for w in ways.iter().filter(|w| only.is_empty() || only.split('|').any(|o| w.name == o)) {
            run(sc, w);
        }
    }
}
