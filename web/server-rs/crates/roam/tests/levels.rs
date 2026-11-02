//! Roads above roads, simulated: free roam's links from the hub and the cars the game shows for them, on a map with a
//! highway 10 m over a street (a meet on each, one straight over the other), a hill of 45 m, and cars hopping over
//! bumps (up to 8 m), with and without the hub's rule for gaps in height (RoamOptions level_*).  Not a test: run it in
//! release,
//!   web/server-rs/cargo.sh roam test --release -p nfsu2-roam --test levels -- --ignored --nocapture
//!
//! Each SNAP has the car's nose on the slope of the ground where it points.  The game is popin.rs's model (slots, handshake, load, the hub's grace, the game's own rank in x and y, as roam_game.c
//! has it).  Measured for each kind of player (driving the highway, driving the streets, parked at the highway's meet,
//! parked at the street's meet under it): of the cars in sight in front of a driver (within 250 m, 40 degrees) and of
//! the ones around anyone (within 100 m), how many show, where a car on the other level within 60 m across is out of
//! sight (the deck is in the way); how many of the cars shown are out of sight; cars in sight that show within 100 m
//! (pop-ins) and that go while within 100 m (pop-outs), a minute a player; and the links made a second a player.

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
/// from a list to a peer (s), from a slot to the car shown (s), roam_core.c T_HUB_GRACE (s)
const HANDSHAKE: f64 = 0.4;
const LOAD: f64 = 1.0;
const GRACE: f64 = 2.5;
/// "in front": this near, and within 40 degrees of where the car drives; "around": this near
const FRONT_M: f64 = 250.0;
const FRONT_COS: f64 = 0.766;
const NEAR_M: f64 = 100.0;
/// out of sight: on the other level, and within this across
const HIDE_M: f64 = 60.0;
/// the map: x and y within HALF; the highway along y = 0, DECK m up, its lanes at y = +-5; streets every 100 m
const HALF: f64 = 800.0;
const DECK: f64 = 10.0;
const BLOCK: f64 = 100.0;
const G: f64 = 9.81;

/// the ground's height: flat, but for a hill of 45 m around (450, 450) (up to 15 % steep)
fn ground(x: f64, y: f64) -> f64 {
    45.0 * (-((x - 450.0).powi(2) + (y - 450.0).powi(2)) / (250.0f64 * 250.0)).exp()
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Highway,
    Street,
    HighwayMeet,
    StreetMeet,
    Shop,
}

const KINDS: [(&str, Kind); 4] =
    [("highway", Kind::Highway), ("streets", Kind::Street), ("highway meet", Kind::HighwayMeet), ("street meet", Kind::StreetMeet)];

#[derive(Clone, Copy)]
struct Car {
    kind: Kind,
    p: [f64; 3],
    v: [f64; 3],
    /// where it points (radians)
    h: f64,
    /// on the streets: along x (0) or y (1), which way (1, -1), how fast; on the highway: which way, how fast
    axis: usize,
    dir: f64,
    speed: f64,
    /// a hop over a bump: since when, and its speed up at the start (0: none)
    hop_at: f64,
    hop_v: f64,
    status: u8,
    next_snap: f64,
}

impl Car {
    fn deck(&self) -> bool {
        matches!(self.kind, Kind::Highway | Kind::HighwayMeet)
    }
    fn drives(&self) -> bool {
        self.status == 2 && (self.v[0] * self.v[0] + self.v[1] * self.v[1]) > 100.0
    }
}

/// across (x and y) between two cars, and whether the one is out of the other's sight
fn across(a: &Car, b: &Car) -> f64 {
    ((a.p[0] - b.p[0]).powi(2) + (a.p[1] - b.p[1]).powi(2)).sqrt()
}
fn hidden(a: &Car, b: &Car) -> bool {
    a.deck() != b.deck() && across(a, b) < HIDE_M
}

/// a peer in a player's game
struct Peer {
    listed: f64,
    first: f64,
    slot: Option<f64>,
    shown: bool,
}

/// the game's rank for its slots (roam_game.c PeerView.rank: x and y only)
fn rank(a: &Car, b: &Car) -> f64 {
    let (dx, dy) = (b.p[0] - a.p[0], b.p[1] - a.p[1]);
    let (ux, uy) = (b.v[0] - a.v[0], b.v[1] - a.v[1]);
    let at = |t: f64| ((dx + ux * t).powi(2) + (dy + uy * t).powi(2)).sqrt();
    let (sx, sy) = (dx + ux - a.h.cos() * 150.0, dy + uy - a.h.sin() * 150.0);
    let view = if a.status == 2 { (sx * sx + sy * sy).sqrt() } else { f64::INFINITY };
    at(1.0).min(at(4.0)).min(view)
}

struct Scene {
    name: &'static str,
    highway: usize,
    street: usize,
    meets: usize,
    shops: usize,
    /// the links (and the game's slots) each player has
    links: usize,
}

#[derive(Default, Clone)]
struct Tally {
    front: u64,
    front_shown: u64,
    around: u64,
    around_shown: u64,
    shown: u64,
    shown_hidden: u64,
    pops: u64,
    outs: u64,
    hop_outs: u64,
    minutes: f64,
}

fn run(scene: &Scene, way: &str, o: &RoamOptions) {
    let mut r = Lcg(1234);
    let mut k = Kit::new(RoamOptions { max_players: 8192, ..o.clone() });
    let vip = |i: usize| 0x0A4D_0100 + i as u32;
    let line = |r: &mut Lcg| ((r.unit() * 2.0 - 1.0) * (HALF / BLOCK - 1.0)).round() * BLOCK;
    let mut cars: Vec<Car> = Vec::new();
    let blank = Car {
        kind: Kind::Street,
        p: [0.0; 3],
        v: [0.0; 3],
        h: 0.0,
        axis: 0,
        dir: 1.0,
        speed: 0.0,
        hop_at: 0.0,
        hop_v: 0.0,
        status: 2,
        next_snap: 0.0,
    };
    for _ in 0..scene.highway {
        let dir = if r.unit() < 0.5 { 1.0 } else { -1.0 };
        let x = (r.unit() * 2.0 - 1.0) * (HALF - 30.0);
        cars.push(Car { kind: Kind::Highway, p: [x, 5.0 * dir, DECK], dir, speed: 30.0 + r.unit() * 20.0, ..blank });
    }
    for _ in 0..scene.street {
        let axis = (r.unit() < 0.5) as usize;
        let mut p = [0.0; 3];
        p[axis] = (r.unit() * 2.0 - 1.0) * HALF;
        p[1 - axis] = line(&mut r);
        let dir = if r.unit() < 0.5 { 1.0 } else { -1.0 };
        cars.push(Car { kind: Kind::Street, p, axis, dir, speed: 10.0 + r.unit() * 15.0, ..blank });
    }
    // the meets: one on the highway's deck, one on the street under it (when there is a highway), each 120 by 18 m
    for kind in [Kind::HighwayMeet, Kind::StreetMeet] {
        if kind == Kind::HighwayMeet && scene.highway == 0 {
            continue;
        }
        for _ in 0..scene.meets {
            let p = [(r.unit() - 0.5) * 120.0, (r.unit() - 0.5) * 18.0, if kind == Kind::HighwayMeet { DECK } else { 0.0 }];
            cars.push(Car { kind, p, h: r.unit() * std::f64::consts::TAU, ..blank });
        }
    }
    for _ in 0..scene.shops {
        let p = [line(&mut r), (r.unit() * 2.0 - 1.0) * HALF, 0.0];
        cars.push(Car { kind: Kind::Shop, p, status: 4, ..blank });
    }
    let n = cars.len();
    for c in cars.iter_mut() {
        c.next_snap = r.unit() * 0.5;
        if !c.deck() {
            c.p[2] += ground(c.p[0], c.p[1]);
        }
    }
    for i in 0..n {
        k.from(vip(i), &hello_with(1000 + i as u32, &HelloOpts { core: 10, links: scene.links as u8, ..Default::default() }));
    }
    let mut games: Vec<HashMap<usize, Peer>> = (0..n).map(|_| HashMap::new()).collect();
    let mut last_list = vec![f64::NEG_INFINITY; n];
    let (warm, end) = (30.0, 150.0);
    let mut tally: Vec<Tally> = vec![Tally::default(); KINDS.len()];
    let class = |c: &Car| KINDS.iter().position(|e| e.1 == c.kind);
    let mut links_made = 0u64;
    let (mut next_tick, mut t, mut step_no) = (0.0, 0.0, 0u64);
    while t < end {
        for c in cars.iter_mut() {
            match c.kind {
                Kind::Highway => {
                    c.p[0] += c.dir * c.speed * STEP;
                    if c.p[0].abs() > HALF - 20.0 {
                        c.dir = -c.dir;
                        c.p[1] = 5.0 * c.dir;
                    }
                    c.v = [c.dir * c.speed, 0.0, 0.0];
                }
                Kind::Street => {
                    let a = c.axis;
                    let old = c.p[a];
                    c.p[a] += c.dir * c.speed * STEP;
                    // a crossing passed: on, or a turn
                    let (b0, b1) = ((old / BLOCK).floor(), (c.p[a] / BLOCK).floor());
                    if b0 != b1 && r.unit() < 0.5 {
                        c.p[a] = b0.max(b1) * BLOCK;
                        c.axis = 1 - a;
                        c.dir = if r.unit() < 0.5 { 1.0 } else { -1.0 };
                    }
                    if c.p[c.axis].abs() > HALF {
                        c.p[c.axis] = c.p[c.axis].signum() * HALF;
                        c.dir = -c.dir;
                    }
                    let mut v = [0.0; 3];
                    v[c.axis] = c.dir * c.speed;
                    // over a bump now and then: up to 8 m, most of them low
                    if c.hop_v == 0.0 && r.unit() < STEP / 20.0 {
                        let apex = 1.0 + 7.0 * r.unit().powi(2);
                        c.hop_v = (2.0 * G * apex).sqrt();
                        c.hop_at = t;
                    }
                    let mut up = 0.0;
                    if c.hop_v > 0.0 {
                        let s = t - c.hop_at;
                        up = c.hop_v * s - G / 2.0 * s * s;
                        if up < 0.0 {
                            (up, c.hop_v) = (0.0, 0.0);
                        }
                    }
                    // its speed up or down: the hill's and the hop's
                    let z = ground(c.p[0], c.p[1]) + up;
                    v[2] = (z - c.p[2]) / STEP;
                    c.p[2] = z;
                    c.v = v;
                }
                _ => {}
            }
            if c.drives() {
                c.h = c.v[1].atan2(c.v[0]);
            }
        }
        for (i, c) in cars.iter_mut().enumerate() {
            if t >= c.next_snap {
                c.next_snap += 0.5;
                // its nose: the ground's slope where it points (on the highway, flat)
                let slope = if c.deck() { 0.0 } else { ground(c.p[0] + c.h.cos(), c.p[1] + c.h.sin()) - ground(c.p[0], c.p[1]) };
                k.from(vip(i), &snap_pose(1000 + i as u32, c.p, c.v, c.h, slope, c.status));
            }
        }
        if t >= next_tick {
            next_tick += o.tick_ms as f64 / 1000.0;
            k.clear();
            k.hub.tick();
            for (key, list) in k.lists() {
                let me = (key.split(':').next().unwrap().parse::<u32>().unwrap() - vip(0)) as usize;
                last_list[me] = t;
                for e in list {
                    let q = (e.vip - vip(0)) as usize;
                    let p = games[me].entry(q).or_insert_with(|| {
                        if t >= warm {
                            links_made += 1;
                        }
                        Peer { listed: t, first: t, slot: None, shown: false }
                    });
                    p.listed = t;
                }
            }
        }
        // each game: peers gone, slots (popin.rs's way: a peer the hub left out gives its slot to one it lists, else
        // the farthest shown gives way to a much nearer one), cars shown
        let measure = t >= warm;
        for me in 0..n {
            let a = cars[me];
            let ll = last_list[me];
            let g = &mut games[me];
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
                .filter(|(q, p)| p.slot.is_none() && cars[**q].status == 2 && t >= p.first + HANDSHAKE && p.listed == ll)
                .map(|(&q, _)| (rank(&a, &cars[q]), q))
                .collect();
            cands.sort_by(|x, y| x.0.total_cmp(&y.0).then(x.1.cmp(&y.1)));
            for (d, q) in cands {
                if used < scene.links {
                    g.get_mut(&q).unwrap().slot = Some(t);
                    used += 1;
                    continue;
                }
                let mut out: Option<(f64, usize)> = None;
                for (&j, p) in g.iter() {
                    if p.slot.is_some() && p.listed != ll {
                        let dj = rank(&a, &cars[j]);
                        if out.is_none_or(|(f, _)| dj > f) {
                            out = Some((dj, j));
                        }
                    }
                }
                if out.is_none() {
                    let mut far: Option<(f64, usize)> = None;
                    for (&j, p) in g.iter() {
                        if p.slot.is_some() {
                            let dj = rank(&a, &cars[j]);
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
            let Some(ci) = class(&a) else { continue };
            for (&q, p) in g.iter_mut() {
                let now = p.slot.is_some_and(|s| t >= s + LOAD);
                if now && !p.shown && measure && a.status == 2 {
                    let b = &cars[q];
                    if !hidden(&a, b) && across(&a, b) < NEAR_M {
                        tally[ci].pops += 1;
                    }
                }
                p.shown = now;
            }
            if measure && a.status == 2 {
                for q in gone {
                    let b = &cars[q];
                    if b.status == 2 && !hidden(&a, b) && across(&a, b) < NEAR_M {
                        tally[ci].outs += 1;
                        tally[ci].hop_outs += (b.hop_v > 0.0) as u64;
                    }
                }
            }
        }
        // what the players see, every half second
        if measure && step_no % 5 == 0 {
            for me in 0..n {
                let a = cars[me];
                let Some(ci) = class(&a) else { continue };
                let tl = &mut tally[ci];
                tl.minutes += 0.5 / 60.0;
                let (fx, fy) = (a.h.cos(), a.h.sin());
                for (q, b) in cars.iter().enumerate() {
                    if q == me || b.status != 2 {
                        continue;
                    }
                    let shown = games[me].get(&q).is_some_and(|p| p.shown);
                    let hide = hidden(&a, b);
                    tl.shown += shown as u64;
                    tl.shown_hidden += (shown && hide) as u64;
                    let d = across(&a, b);
                    if hide || d == 0.0 || d > FRONT_M {
                        continue;
                    }
                    if d < NEAR_M {
                        tl.around += 1;
                        tl.around_shown += shown as u64;
                    }
                    let cos = ((b.p[0] - a.p[0]) * fx + (b.p[1] - a.p[1]) * fy) / d;
                    if a.drives() && cos > FRONT_COS {
                        tl.front += 1;
                        tl.front_shown += shown as u64;
                    }
                }
            }
        }
        k.step(STEP * 1000.0);
        t += STEP;
        step_no += 1;
    }
    let pc = |a: u64, b: u64| if b == 0 { f64::NAN } else { 100.0 * a as f64 / b as f64 };
    for (ci, (name, _)) in KINDS.iter().enumerate() {
        let tl = &tally[ci];
        if tl.minutes == 0.0 {
            continue;
        }
        println!(
            "{:<8} {:<18} {:<13} in front {:>5.1} %, around {:>5.1} %, shown out of sight {:>5.1} %, pop-ins {:>5.2}/min, \
             pop-outs {:>5.2}/min ({:.2} hopping)",
            scene.name,
            way,
            name,
            pc(tl.front_shown, tl.front),
            pc(tl.around_shown, tl.around),
            pc(tl.shown_hidden, tl.shown),
            tl.pops as f64 / tl.minutes,
            tl.outs as f64 / tl.minutes,
            tl.hop_outs as f64 / tl.minutes,
        );
    }
    println!("{:<8} {:<18} links {:.2}/s a player", scene.name, way, links_made as f64 / (n as f64 * (end - warm)));
}

/// the hub's ways compared: without the rule (a gap counts as it is), the rule as it is, and with other numbers
fn ways() -> Vec<(&'static str, RoamOptions)> {
    let d = RoamOptions::default();
    vec![
        ("no rule", RoamOptions { level_k: 1e-9, ..d.clone() }),
        ("rule", d.clone()),
        ("rule, k 10", RoamOptions { level_k: 10.0, ..d.clone() }),
        ("rule, k 50", RoamOptions { level_k: 50.0, ..d.clone() }),
        ("rule, slack 3", RoamOptions { level_slack_m: 3.0, ..d.clone() }),
        ("rule, grade 0.3", RoamOptions { level_grade: 0.3, ..d.clone() }),
    ]
}

#[test]
#[ignore]
fn levels() {
    let scenes = [
        Scene { name: "highway", highway: 300, street: 600, meets: 60, shops: 60, links: 32 },
        Scene { name: "city", highway: 0, street: 900, meets: 60, shops: 60, links: 32 },
        Scene { name: "phones", highway: 300, street: 600, meets: 60, shops: 60, links: 16 },
    ];
    let only = std::env::var("LEVELS_WAYS").ok().filter(|w| !w.is_empty());
    for s in &scenes {
        for (name, o) in ways() {
            if only.as_deref().is_some_and(|w| !w.split(',').any(|x| x == name)) {
                continue;
            }
            run(s, name, &o);
        }
    }
}
