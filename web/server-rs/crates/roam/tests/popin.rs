//! Pop-in in a crowd: free roam's links from the hub and the cars the game shows for them, simulated, so that the
//! hub's options (and the game's way with its slots) can be compared.  Not a test: run it in release,
//!   web/server-rs/cargo.sh roam test --release -p nfsu2-roam --test popin -- --ignored --nocapture
//!
//! The players: some drive straight on (now and then a turn), some are parked, some are in a shop.  Each sends the
//! hub a SNAP twice a second, each at its own phase, and the hub ticks every `tick_ms`.  The game of each player is a
//! model of roam_core and roam_game.c's slots: a player listed is a peer after a handshake, a peer in free roam gets
//! one of 32 slots (the nearest first, by the game's own rank), and its car shows once loaded; a peer the hub left out
//! goes after T_HUB_GRACE.  Measured for the players driving: of the cars in front of them (within 250 m, 40 degrees
//! of where they drive), how many show; how near a car is when it shows there (a pop-in); how far behind a car is when
//! it goes; and how many links a second the hub makes (each a handshake and a car loaded).

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
}

/// the simulation's step (s)
const STEP: f64 = 0.1;
/// the game's slots (roam_game.c NSLOT)
const SLOTS: usize = 32;
/// from a list to a peer: the list on its way and HELLOs both ways through the switch (s)
const HANDSHAKE: f64 = 0.4;
/// from a slot to the car shown: the ride loaded (a look the car loader has seen) and the car made (s)
const LOAD: f64 = 1.0;
/// roam_core.c T_HUB_GRACE (s)
const GRACE: f64 = 2.5;
/// "in front": this near, and within 40 degrees of where the car drives
const VIEW_M: f64 = 250.0;
const VIEW_COS: f64 = 0.766;
/// a pop-in: a car that shows in front this near
const POP_M: f64 = 100.0;
/// a driver turns now and then, a quarter turn either way at most, at this rate (radians a second: 60 degrees)
const TURN_RATE: f64 = 1.047;

#[derive(Clone, Copy)]
struct Car {
    p: [f64; 3],
    v: [f64; 3],
    /// where it points (radians): where it drives, or where it was parked
    h: f64,
    /// what is left of the turn it is in (radians), turned at TURN_RATE
    turn: f64,
    status: u8,
    next_snap: f64,
}

impl Car {
    fn drives(&self) -> bool {
        self.status == 2 && (self.v[0] * self.v[0] + self.v[1] * self.v[1]) > 100.0
    }
}

/// a peer in a player's game
struct Peer {
    /// the last list it was on
    listed: f64,
    /// its first list (a peer since then plus the handshake)
    first: f64,
    /// since when it has a slot
    slot: Option<f64>,
    shown: bool,
}

/// how the game ranks its peers for the slots (roam_game.c PeerView.rank): the nearest of the distances `soon` and
/// `look` seconds on, both cars at their speeds, and from its view point `view` m ahead (soon) to the other car (x and
/// y)
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
    tick_ms: f64,
    rank: Rank,
    /// a peer the hub left out gives its slot to one it lists (else it keeps it until it goes, or the game's step 5)
    give_way: bool,
}

struct Scene {
    name: &'static str,
    n: usize,
    side: f64,
}

#[derive(Default)]
struct Tally {
    front: u64,
    front_shown: u64,
    near: u64,
    near_shown: u64,
    pops: u64,
    appear_front: Vec<f64>,
    gone_behind: Vec<f64>,
    behind_shown: u64,
    samples: u64,
    parked_front: u64,
    parked_front_shown: u64,
    links_made: u64,
}

fn median(v: &mut [f64]) -> f64 {
    if v.is_empty() {
        return f64::NAN;
    }
    v.sort_by(f64::total_cmp);
    v[v.len() / 2]
}

fn run(scene: &Scene, way: &Way) {
    let mut r = Lcg(42);
    let mut k = Kit::new(RoamOptions { max_players: 8192, ..way.o.clone() });
    let vip = |i: usize| 0x0A4D_0100 + i as u32;
    let half = scene.side / 2.0;
    let mut cars: Vec<Car> = (0..scene.n)
        .map(|_| {
            let p = [(r.unit() - 0.5) * scene.side, (r.unit() - 0.5) * scene.side, 0.0];
            let kind = r.unit();
            let (a, s) = (r.unit() * std::f64::consts::TAU, 15.0 + r.unit() * 35.0);
            let (v, status) = if kind < 0.55 {
                ([a.cos() * s, a.sin() * s, 0.0], 2)
            } else if kind < 0.9 {
                ([0.0; 3], 2)
            } else {
                ([0.0; 3], 4)
            };
            Car { p, v, h: a, turn: 0.0, status, next_snap: r.unit() * 0.5 }
        })
        .collect();
    for i in 0..scene.n {
        k.from(vip(i), &hello_with(1000 + i as u32, &HelloOpts { core: 4, ..Default::default() }));
    }
    let mut games: Vec<HashMap<usize, Peer>> = (0..scene.n).map(|_| HashMap::new()).collect();
    let mut last_list = vec![f64::NEG_INFINITY; scene.n];
    let (warm, end) = (30.0, 150.0);
    let mut tally = Tally::default();
    let mut next_tick = 0.0;
    let mut t = 0.0;
    let mut step_no = 0u64;
    while t < end {
        // the cars move; a driver turns now and then
        for c in cars.iter_mut() {
            if c.drives() && c.turn == 0.0 && r.unit() < STEP / 6.0 {
                c.turn = (r.unit() - 0.5) * std::f64::consts::PI;
            }
            if c.turn != 0.0 {
                let a = c.turn.clamp(-TURN_RATE * STEP, TURN_RATE * STEP);
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
            if c.drives() {
                c.h = c.v[1].atan2(c.v[0]);
            }
        }
        // their SNAPs to the hub
        for (i, c) in cars.iter_mut().enumerate() {
            if t >= c.next_snap {
                c.next_snap += 0.5;
                k.from(vip(i), &snap_moving(1000 + i as u32, c.p, c.v, c.h, c.status));
            }
        }
        if t >= next_tick {
            next_tick += way.tick_ms / 1000.0;
            k.clear();
            k.hub.tick();
            for (key, list) in k.lists() {
                let me = (key.split(':').next().unwrap().parse::<u32>().unwrap() - vip(0)) as usize;
                last_list[me] = t;
                for e in list {
                    let q = (e.vip - vip(0)) as usize;
                    let g = &mut games[me];
                    let p = g.entry(q).or_insert_with(|| {
                        if t >= warm {
                            tally.links_made += 1;
                        }
                        Peer { listed: t, first: t, slot: None, shown: false }
                    });
                    p.listed = t;
                }
            }
        }
        // each game: peers gone, slots, cars shown
        for me in 0..scene.n {
            let g = &mut games[me];
            let (a, ll) = (cars[me], last_list[me]);
            let mut gone = Vec::new();
            g.retain(|&q, p| {
                let keep = t - p.listed <= GRACE;
                if !keep && p.shown {
                    gone.push(q);
                }
                keep
            });
            let mut used = g.values().filter(|p| p.slot.is_some()).count();
            let mut cands: Vec<(f64, usize)> = g
                .iter()
                .filter(|(q, p)| {
                    p.slot.is_none() && cars[**q].status == 2 && t >= p.first + HANDSHAKE && (!way.give_way || p.listed == ll)
                })
                .map(|(&q, _)| (way.rank.of(&a, &cars[q]), q))
                .collect();
            cands.sort_by(|x, y| x.0.total_cmp(&y.0).then(x.1.cmp(&y.1)));
            for (d, q) in cands {
                if used < SLOTS {
                    g.get_mut(&q).unwrap().slot = Some(t);
                    used += 1;
                    continue;
                }
                // a slot to take: one of a peer the hub left out (the farthest), else the farthest shown one for a
                // much nearer peer (step 5)
                let mut out: Option<(f64, usize)> = None;
                if way.give_way {
                    for (&j, p) in g.iter() {
                        if p.slot.is_some() && p.listed != ll {
                            let dj = way.rank.of(&a, &cars[j]);
                            if out.is_none_or(|(f, _)| dj > f) {
                                out = Some((dj, j));
                            }
                        }
                    }
                }
                if out.is_none() {
                    let mut far: Option<(f64, usize)> = None;
                    for (&j, p) in g.iter() {
                        if p.slot.is_some() {
                            let dj = way.rank.of(&a, &cars[j]);
                            if far.is_none_or(|(f, _)| dj > f) {
                                far = Some((dj, j));
                            }
                        }
                    }
                    out = far.filter(|&(f, _)| d < f * 0.6 && f - d > 50.0);
                }
                let Some((_, j)) = out else { break };
                let pj = g.get_mut(&j).unwrap();
                pj.slot = None;
                if pj.shown {
                    pj.shown = false;
                    gone.push(j);
                }
                g.get_mut(&q).unwrap().slot = Some(t);
            }
            let measure = t >= warm && a.drives();
            let (fx, fy) = (a.h.cos(), a.h.sin());
            let rel = |q: usize| {
                let (dx, dy) = (cars[q].p[0] - a.p[0], cars[q].p[1] - a.p[1]);
                let d = (dx * dx + dy * dy).sqrt();
                (d, if d > 0.0 { (dx * fx + dy * fy) / d } else { 1.0 })
            };
            for (&q, p) in g.iter_mut() {
                let now = p.slot.is_some_and(|s| t >= s + LOAD);
                if now && !p.shown && measure {
                    let (d, cos) = rel(q);
                    if cos > VIEW_COS && d < VIEW_M {
                        tally.appear_front.push(d);
                        if d < POP_M {
                            tally.pops += 1;
                        }
                    }
                }
                p.shown = now;
            }
            if measure {
                for q in gone {
                    let (d, cos) = rel(q);
                    if cos < 0.0 {
                        tally.gone_behind.push(d);
                    }
                }
            }
        }
        // what the players see, every half second: the drivers, and the parked ones in free roam
        if t >= warm && step_no % 5 == 0 {
            for me in 0..scene.n {
                let a = cars[me];
                let drives = a.drives();
                if a.status != 2 {
                    continue;
                }
                tally.samples += drives as u64;
                let (fx, fy) = (a.h.cos(), a.h.sin());
                for (q, b) in cars.iter().enumerate() {
                    if q == me || b.status != 2 {
                        continue;
                    }
                    let (dx, dy) = (b.p[0] - a.p[0], b.p[1] - a.p[1]);
                    let d = (dx * dx + dy * dy).sqrt();
                    if d > VIEW_M || d == 0.0 {
                        continue;
                    }
                    let cos = (dx * fx + dy * fy) / d;
                    let shown = games[me].get(&q).is_some_and(|p| p.shown);
                    if !drives {
                        if cos > VIEW_COS {
                            tally.parked_front += 1;
                            tally.parked_front_shown += shown as u64;
                        }
                        continue;
                    }
                    if cos > VIEW_COS {
                        tally.front += 1;
                        tally.front_shown += shown as u64;
                        if d < POP_M {
                            tally.near += 1;
                            tally.near_shown += shown as u64;
                        }
                    }
                    if cos < -0.5 && d > 100.0 && shown {
                        tally.behind_shown += 1;
                    }
                }
            }
        }
        k.step(STEP * 1000.0);
        t += STEP;
        step_no += 1;
    }
    let drivers = cars.iter().filter(|c| c.drives()).count() as f64;
    let minutes = drivers * (end - warm) / 60.0;
    let pc = |a: u64, b: u64| 100.0 * a as f64 / b.max(1) as f64;
    println!(
        "{:>5} {:<26} front {:>4.1} % (<100 m {:>4.1} %), pops <100 m {:>5.2}/min, appear {:>3.0} m, gone {:>3.0} m behind, \
         {:>3.1} far behind | parked: front {:>4.1} % | links {:.2}/s",
        scene.name,
        way.name,
        pc(tally.front_shown, tally.front),
        pc(tally.near_shown, tally.near),
        tally.pops as f64 / minutes,
        median(&mut tally.appear_front),
        median(&mut tally.gone_behind),
        tally.behind_shown as f64 / tally.samples.max(1) as f64,
        pc(tally.parked_front_shown, tally.parked_front),
        tally.links_made as f64 / (scene.n as f64 * (end - warm)),
    );
}

#[test]
#[ignore]
fn popin() {
    let scenes = [Scene { name: "city", n: 1400, side: 3000.0 }, Scene { name: "crowd", n: 600, side: 800.0 }];
    // the hub before 2026-10-23 (an option that is not a positive number keeps its default: 1e-9 is none), and the
    // game's slots then: a peer the hub left out kept its slot until it went
    let off = 1e-9;
    let old = RoamOptions { tick_ms: 1000, soon_ms: off, look_ms: 3000.0, away_m: off, view_m: off, ..RoamOptions::default() };
    let game_old = Rank { soon: 0.0, look: 3.0, view: 0.0 };
    let new = RoamOptions::default();
    let game = |o: &RoamOptions| Rank { soon: o.soon_ms / 1000.0, look: o.look_ms / 1000.0, view: o.view_m };
    let way = |name, o: RoamOptions, give_way| Way { name, tick_ms: o.tick_ms as f64, rank: game(&o), o, give_way };
    let ways = [
        Way { name: "before", tick_ms: 1000.0, rank: game_old, o: old, give_way: false },
        way("now", new.clone(), true),
        Way { name: "now, the game as before", tick_ms: 500.0, rank: game_old, o: new.clone(), give_way: false },
        way("now, tick 1 s", RoamOptions { tick_ms: 1000, ..new.clone() }, true),
        way("now, no view point", RoamOptions { view_m: off, ..new.clone() }, true),
        way("now, view 100 m", RoamOptions { view_m: 100.0, ..new.clone() }, true),
        way("now, view 200 m", RoamOptions { view_m: 200.0, ..new.clone() }, true),
        way("now, soon 0 s", RoamOptions { soon_ms: off, ..new.clone() }, true),
        way("now, look 3 s", RoamOptions { look_ms: 3000.0, ..new.clone() }, true),
        way("now, look 5 s", RoamOptions { look_ms: 5000.0, ..new.clone() }, true),
        way("now, nobody away", RoamOptions { away_m: off, ..new.clone() }, true),
        way("now, keep 0.7", RoamOptions { keep: 0.7, ..new.clone() }, true),
    ];
    for s in &scenes {
        for w in &ways {
            run(s, w);
        }
    }
}
