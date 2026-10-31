//! What the hub does that the Node server's hub never did: links that look ahead at the cars' speeds, and the world
//! map's answer (MAPREQ / MAPD).

mod kit;

use std::path::{Path, PathBuf};
use std::process::Command;

use kit::*;
use nfsu2_roam::RoamOptions;

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../..")
}

/// browser i's address, for more players than V's: 10.77.0.2 and on
#[allow(non_snake_case)]
fn W(i: u32) -> u32 {
    0x0A4D_0002 + i
}

/// a player of core version 4 (32 links) in room 0
fn join(k: &mut Kit, i: u32) {
    k.from(W(i), &hello_with(100 + i, &HelloOpts { core: 4, ..Default::default() }));
}

// a car heading for a player far ahead is linked to it before the parked ones beside it, and the one it drives away
// from gives way first; parked, the nearest ones
#[test]
fn links_look_ahead_at_the_cars_speeds() {
    for (how, o) in ways() {
        for moving in [false, true] {
            let mut k = Kit::new(RoamOptions { degree: 2, ..o.clone() });
            for i in 0..4 {
                join(&mut k, i);
            }
            // 0 at 50 m/s along x, 1 parked 200 m ahead of it (where 0 is in 4 s), 2 parked 40 m behind it (90 m in
            // a second), 3 parked 45 m beside it (67 m in a second)
            let v = if moving { 50.0 } else { 0.0 };
            k.from(W(0), &snap_moving(100, [0.0, 0.0, 0.0], [v, 0.0, 0.0], 0.0, 2));
            k.from(W(1), &snap(101, [200.0, 0.0, 0.0]));
            k.from(W(2), &snap(102, [-40.0, 0.0, 0.0]));
            k.from(W(3), &snap(103, [0.0, -45.0, 0.0]));
            k.clear();
            k.hub.tick();
            let l = k.lists();
            let has = |i: u32, j: u32| l[&format!("{}:47210", W(i))].iter().any(|e| e.vip == W(j));
            assert_eq!(has(0, 1), moving, "{how}, moving {moving}: 0 and the player it heads for");
            assert_eq!(has(0, 2), !moving, "{how}, moving {moving}: 0 and the one behind it");
            assert!(has(0, 3), "{how}, moving {moving}: 0 and the one beside it");
        }
    }
}

// a car in front of where a player's car points is linked from farther away (from its view point): 250 m in front
// before 120 m behind; turned around, the one behind it; without view points, the nearer one
#[test]
fn links_see_farther_in_front() {
    for (how, o) in ways() {
        for (heading, view, want) in [(0.0, true, 1), (std::f64::consts::PI, true, 2), (0.0, false, 2)] {
            let mut k = Kit::new(RoamOptions { degree: 1, view_m: if view { 150.0 } else { o.view_m }, ..o.clone() });
            for i in 0..3 {
                join(&mut k, i);
            }
            // 1 points on along x, 2 away from 0 (their view points far from 0)
            k.from(W(0), &snap_moving(100, [0.0, 0.0, 0.0], [0.0; 3], heading, 2));
            k.from(W(1), &snap_moving(101, [250.0, 0.0, 0.0], [0.0; 3], 0.0, 2));
            k.from(W(2), &snap_moving(102, [-120.0, 0.0, 0.0], [0.0; 3], std::f64::consts::PI, 2));
            k.hub.tick();
            let l = k.lists();
            assert_eq!(
                l[&format!("{}:47210", W(0))].iter().map(|e| e.vip).collect::<Vec<_>>(),
                [W(want)],
                "{how}: heading {heading}, view {view}"
            );
        }
    }
}

// a player not in free roam (a shop) counts 100 m up: a car in free roam 80 m away comes before it at 10 m; paused in
// free roam it counts where it is
#[test]
fn links_go_to_the_cars_shown_first() {
    for (how, o) in ways() {
        for (status, want) in [(4u8, 2), (2, 1), (5, 1), (3, 2)] {
            let mut k = Kit::new(RoamOptions { degree: 1, ..o.clone() });
            for i in 0..3 {
                join(&mut k, i);
            }
            k.from(W(0), &snap(100, [0.0, 0.0, 0.0]));
            k.from(W(1), &snap_n(101, [10.0, 0.0, 0.0], status, 1));
            k.from(W(2), &snap(102, [80.0, 0.0, 0.0]));
            k.hub.tick();
            let l = k.lists();
            assert_eq!(l[&format!("{}:47210", W(0))].iter().map(|e| e.vip).collect::<Vec<_>>(), [W(want)], "{how}: status {status}");
        }
    }
}

// a car's place is moved on by its SNAP's age (up to a second): heading for 0 at 50 m/s from 400 m, it is 150 m from
// 0 in 4 s once its SNAP is a second old (before a parked one 175 m away), 200 m when it is new; a paused game's speed
// moves nothing
#[test]
fn links_move_a_car_on_by_its_snaps_age() {
    for (how, o) in ways() {
        for (age, status, want) in [(0.0, 2u8, 2), (1000.0, 2, 1), (3000.0, 2, 1), (1000.0, 5, 2)] {
            let mut k = Kit::new(RoamOptions { degree: 1, ..o.clone() });
            for i in 0..3 {
                join(&mut k, i);
            }
            k.from(W(0), &snap(100, [0.0, 0.0, 0.0]));
            k.from(W(1), &snap_moving(101, [400.0, 0.0, 0.0], [-50.0, 0.0, 0.0], std::f64::consts::PI, status));
            k.from(W(2), &snap(102, [0.0, 175.0, 0.0]));
            k.step(age);
            // (0 and 2 just sent theirs)
            k.from(W(0), &snap(100, [0.0, 0.0, 0.0]));
            k.from(W(2), &snap(102, [0.0, 175.0, 0.0]));
            k.hub.tick();
            let l = k.lists();
            assert_eq!(
                l[&format!("{}:47210", W(0))].iter().map(|e| e.vip).collect::<Vec<_>>(),
                [W(want)],
                "{how}: a SNAP {age} ms old, status {status}"
            );
        }
    }
}

/// a small random number generator (the tests' own, the same every run)
struct Rng(u64);
impl Rng {
    fn unit(&mut self) -> f64 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        (self.0 >> 11) as f64 / (1u64 << 53) as f64
    }
}

// the grid's three walks (soon, ahead, and from the view points) give the links every pair sorted gives, tick after
// tick, with players driving about, parking, a few on one spot, some in a shop or paused, and links held and kept
#[test]
fn the_grid_links_as_every_pair_with_the_cars_moving() {
    let n = 400u32;
    let mut r = Rng(11);
    let mut every = Kit::new(RoamOptions { exact_room: 1000, ..RoamOptions::default() });
    let mut grid = Kit::new(RoamOptions { exact_room: 1, ..RoamOptions::default() });
    let mut cars: Vec<([f64; 3], [f64; 3])> = (0..n)
        .map(|i| {
            let p = if i % 20 == 0 { [100.0, 100.0, 0.0] } else { [(r.unit() - 0.5) * 3000.0, (r.unit() - 0.5) * 3000.0, r.unit() * 10.0] };
            let v = if i % 3 == 0 { [0.0; 3] } else { [(r.unit() - 0.5) * 120.0, (r.unit() - 0.5) * 120.0, 0.0] };
            (p, v)
        })
        .collect();
    for k in [&mut every, &mut grid] {
        for i in 0..n {
            join(k, i);
        }
    }
    for tick in 0..12 {
        for (i, (p, v)) in cars.iter_mut().enumerate() {
            for a in 0..2 {
                p[a] = (p[a] + v[a]).clamp(-19000.0, 19000.0);
            }
            if r.unit() < 0.1 {
                *v = [(r.unit() - 0.5) * 120.0, (r.unit() - 0.5) * 120.0, 0.0];
            }
            // some in a shop, some paused (their speeds move nothing, their view points none)
            let status = if i % 7 == 0 { 4 } else if i % 11 == 0 { 5 } else { 2 };
            let d = snap_moving(100 + i as u32, *p, *v, r.unit() * 6.0, status);
            every.from(W(i as u32), &d);
            grid.from(W(i as u32), &d);
        }
        for k in [&mut every, &mut grid] {
            k.clear();
            k.hub.tick();
            k.step(1000.0);
        }
        let (a, b) = (every.lists(), grid.lists());
        assert_eq!(a.len(), n as usize);
        for (key, l) in &a {
            let mut x: Vec<u32> = l.iter().map(|e| e.vip).collect();
            let mut y: Vec<u32> = b[key].iter().map(|e| e.vip).collect();
            x.sort();
            y.sort();
            assert_eq!(x, y, "tick {tick}, player {key}");
        }
    }
}

/// the MAPD datagrams the hub sent since the last clear
fn mapds(k: &Kit) -> Vec<(u32, Mapd)> {
    k.sent.lock().iter().filter_map(|(vip, f)| mapd_of(f).map(|m| (*vip, m))).collect()
}

/// the entries of one MAPD answer, its parts in order (each checked to be of it): (req, base, total, entries)
fn mapd_answer(k: &Kit) -> (u16, u16, u16, Vec<MapdEntry>) {
    let m = mapds(k);
    assert!(!m.is_empty(), "an answer");
    let (req, base, n, total) = (m[0].1.0, m[0].1.1, m[0].1.3, m[0].1.4);
    assert!(m.iter().enumerate().all(|(j, (_, x))| (x.0, x.1, x.2 as usize, x.3, x.4) == (req, base, j, n, total)), "one answer's parts");
    (req, base, total, m.into_iter().flat_map(|(_, x)| x.5).collect())
}

// the world map: the players of the room in free roam, past the one asking and its links; not those in a race, another
// room or gone quiet (5 s); a player alone in its room gets one empty datagram, a stranger (no HELLO) nothing
#[test]
fn the_world_map_shows_the_room_in_free_roam_lately() {
    let mut k = Kit::new(RoamOptions { degree: 1, ..RoamOptions::default() });
    for i in 0..10 {
        join(&mut k, i);
    }
    k.from(W(10), &hello_with(110, &HelloOpts { room: fnv1a("blue"), ..Default::default() }));
    // player i at x = 10 i (rounded), heading a quarter turn (64)
    for i in 0..=10u32 {
        let status = if i == 7 { 3 } else { 2 };
        k.from(W(i), &snap_moving(100 + i, [i as f64 * 10.0 + 0.25, 3.75, 0.0], [0.0; 3], std::f64::consts::FRAC_PI_2, status));
    }
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq_base(100, 1, 2048, 0));
    // its link is 1 (the nearest), 7 races, 10 is in another room
    let want: Vec<MapdEntry> = (2..10u16).filter(|&i| i != 7).map(|i| (i, Some((i as i16 * 10, 4, 64, 0, 0)))).collect();
    assert_eq!(mapd_answer(&k), (1, 0, 7, want));
    // a player alone in its room: one empty datagram; a stranger (no HELLO): nothing
    k.clear();
    k.from(W(10), &mapreq_base(110, 1, 2048, 0));
    assert_eq!(mapds(&k).into_iter().map(|(vip, x)| (vip, x.0, x.1, x.2, x.3, x.4, x.5.len())).collect::<Vec<_>>(), [(W(10), 1, 0, 0, 1, 0, 0)]);
    k.clear();
    k.from(W(20), &mapreq_base(120, 1, 2048, 0));
    assert!(mapds(&k).is_empty());
    // positions older than 5 s are not shown
    k.step(5100.0);
    k.from(W(1), &snap(101, [10.0, 0.0, 0.0]));
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq_base(100, 2, 2048, 0));
    assert_eq!(mapd_answer(&k).2, 0, "only its link 1 is fresh");
}

// one answer a quarter second for a browser, whatever its endpoints: a BYE and a HELLO, or another port, make a new
// player, not a new answer (the second security audit: that way a few clients kept the hub's thread busy with maps)
#[test]
fn the_world_map_answers_a_browser_once_a_quarter_second_whatever_its_endpoints() {
    let mut k = Kit::new(RoamOptions::default());
    for i in 0..20 {
        join(&mut k, i);
        k.from(W(i), &snap(100 + i, [i as f64 * 10.0, 0.0, 0.0]));
    }
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq_base(100, 1, 2048, 0));
    assert_eq!(mapds(&k).len(), 1);
    // again as a new player on the same endpoint, then on other ports: nothing within the quarter second
    k.clear();
    k.from(W(0), &bye(100));
    join(&mut k, 0);
    k.from(W(0), &mapreq_base(100, 2, 2048, 0));
    for port in [47211, 47212] {
        k.hub.frame(W(0), &frame_ports(W(0), &hello(100), port, 47210));
        k.hub.frame(W(0), &frame_ports(W(0), &mapreq_base(100, 3, 2048, 0), port, 47210));
    }
    assert!(mapds(&k).is_empty(), "one answer a quarter second for the browser");
    // another browser is not held back by it; and past the quarter second (and a tick, which forgets old answers) it
    // asks again
    k.from(W(1), &mapreq_base(101, 1, 2048, 0));
    assert_eq!(mapds(&k).len(), 1);
    k.clear();
    k.step(250.0);
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq_base(100, 4, 2048, 0));
    assert_eq!(mapds(&k).len(), 1);
}

// a page since MAPD gets what changed since the answer it holds: everyone at first (by slot, with their velocities),
// then only the players come, gone, or off where the page moved them on at their velocities; from nothing again when
// the hub has no such answer; one answer a quarter second
#[test]
fn the_world_map_sends_what_changed() {
    let mut k = Kit::new(RoamOptions { degree: 1, ..RoamOptions::default() });
    for i in 0..8 {
        join(&mut k, i);
    }
    // 0 asks; 1 its link; 2, 3 stand; 4 drives east at 20 m/s, 5 north at 10; 6 drives east, then turns; 7 is to go
    let at = |i: u32, t: f64| -> ([f64; 3], [f64; 3]) {
        match i {
            4 => ([400.0 + 20.0 * t, 0.0, 0.0], [20.0, 0.0, 0.0]),
            5 => ([500.0, 10.0 * t, 0.0], [0.0, 10.0, 0.0]),
            6 if t > 0.0 => ([600.0 + 20.0 * t, 15.0, 0.0], [20.0, 0.0, 0.0]),
            6 => ([600.0, 0.0, 0.0], [20.0, 0.0, 0.0]),
            _ => ([i as f64 * 100.0, 0.0, 0.0], [0.0; 3]),
        }
    };
    let snaps = |k: &mut Kit, t: f64| {
        for i in 0..8 {
            let (p, v) = at(i, t);
            k.from(W(i), &snap_moving(100 + i, p, v, 0.0, 2));
        }
    };
    snaps(&mut k, 0.0);
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq_base(100, 1, 2048, 0));
    let first = mapd_answer(&k);
    assert_eq!(
        first,
        (
            1,
            0,
            6,
            vec![
                (2, Some((200, 0, 0, 0, 0))),
                (3, Some((300, 0, 0, 0, 0))),
                (4, Some((400, 0, 0, 20, 0))),
                (5, Some((500, 0, 0, 0, 10))),
                (6, Some((600, 0, 0, 20, 0))),
                (7, Some((700, 0, 0, 0, 0)))
            ]
        ),
        "everyone past itself and its link, by slot"
    );
    // a quarter second on: 4 and 5 where the page has them, 6 turned off 15 m, 7 gone, 8 come
    k.step(250.0);
    k.from(W(7), &bye(107));
    join(&mut k, 8);
    snaps(&mut k, 0.25);
    k.from(W(8), &snap(108, [-50.0, 0.0, 0.0]));
    k.hub.tick();
    k.clear();
    k.step(100.0);
    k.from(W(0), &mapreq_base(100, 2, 2048, 1));
    // (6 where it is at the question, 100 ms after its SNAP)
    assert_eq!(mapd_answer(&k), (2, 1, 6, vec![(6, Some((607, 15, 0, 20, 0))), (7, None), (8, Some((-50, 0, 0, 0, 0)))]));
    // within the quarter second: nothing
    k.clear();
    k.step(100.0);
    k.from(W(0), &mapreq_base(100, 3, 2048, 2));
    assert!(mapds(&k).is_empty(), "one answer a quarter second");
    // the page did not take answer 2: it asks from 1 again and gets the same
    k.clear();
    k.step(200.0);
    k.from(W(0), &mapreq_base(100, 4, 2048, 1));
    let again = mapd_answer(&k);
    assert_eq!((again.1, again.3.iter().map(|e| e.0).collect::<Vec<_>>()), (1, vec![6, 7, 8]));
    // an answer the hub does not have: from nothing
    k.clear();
    k.step(300.0);
    k.from(W(0), &mapreq_base(100, 5, 2048, 77));
    let full = mapd_answer(&k);
    assert_eq!((full.0, full.1, full.3.len()), (5, 0, 6));
    // from that one on: what changed since it
    k.clear();
    k.step(300.0);
    k.from(W(0), &mapreq_base(100, 6, 2048, 5));
    assert_eq!(mapd_answer(&k).1, 5);
    // a page that asks for fewer gets the nearest, and the others go
    k.clear();
    k.step(300.0);
    k.from(W(0), &mapreq_base(100, 7, 2, 6));
    let few = mapd_answer(&k);
    assert_eq!((few.1, few.2), (6, 6));
    assert_eq!(few.3.iter().filter(|e| e.1.is_some()).count(), 0, "{few:?}");
    assert_eq!(few.3.iter().filter(|e| e.1.is_none()).map(|e| e.0).collect::<Vec<_>>(), [3, 4, 5, 6], "the nearest, 8 and 2, stay");
}

// a car going straight is placed once: its SNAPs come twice a second, the questions four times, each SNAP's
// position as old as it is when asked (moved on at its velocity, it is where the page has the marker); one turning
// is placed again
#[test]
fn the_world_map_places_a_car_going_straight_once() {
    let mut k = Kit::new(RoamOptions { degree: 1, ..RoamOptions::default() });
    for i in 0..4 {
        join(&mut k, i);
    }
    k.from(W(0), &snap(100, [0.0, 0.0, 0.0]));
    k.from(W(1), &snap(101, [5.0, 0.0, 0.0]));
    let (mut placed, mut base) = (Vec::new(), 0u16);
    for step in 0..40u32 {
        let t = step as f64 * 0.125;
        // 2 straight east at 40 m/s, 3 north at 30 m/s until 2 s, then east; a SNAP each half second, 3's a quarter after 2's
        if step % 4 == 1 {
            k.from(W(2), &snap_moving(102, [100.0 + 40.0 * t, 0.0, 0.0], [40.0, 0.0, 0.0], 0.0, 2));
        }
        if step % 4 == 3 {
            let (p, v) = if t < 2.0 { ([0.0, 100.0 + 30.0 * t, 0.0], [0.0, 30.0, 0.0]) } else { ([30.0 * (t - 2.0), 160.0, 0.0], [30.0, 0.0, 0.0]) };
            k.from(W(3), &snap_moving(103, p, v, 0.0, 2));
        }
        if step % 2 == 0 {
            k.hub.tick();
            k.clear();
            k.from(W(0), &mapreq_base(100, step as u16 + 1, 2048, base));
            let a = mapd_answer(&k);
            base = a.0;
            placed.extend(a.3.into_iter().filter(|e| e.0 >= 2).map(|e| (step, e.0)));
        }
        k.step(125.0);
    }
    assert_eq!(placed.iter().filter(|p| p.1 == 2).count(), 1, "{placed:?}");
    let turned: Vec<u32> = placed.iter().filter(|p| p.1 == 3).map(|p| p.0).collect();
    assert!(turned.len() == 2 && turned[0] <= 4 && (16..=22).contains(&turned[1]), "{placed:?}");
}

// MAPD from nothing for 2048 of 2200: 16 full datagrams
#[test]
fn the_world_map_sends_2048_in_16_mapd_datagrams() {
    let mut k = Kit::new(RoamOptions { degree: 1, ..RoamOptions::default() });
    for i in 0..2200 {
        join(&mut k, i);
        k.from(W(i), &snap(100 + i, [i as f64 * 5.0, 0.0, 0.0]));
    }
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq_base(100, 1, 2048, 0));
    let m = mapds(&k);
    assert_eq!(m.len(), 16);
    assert!(m.iter().all(|(_, x)| x.5.len() == 128));
    let (_, _, total, all) = mapd_answer(&k);
    assert_eq!((total, all.len()), (2198, 2048));
    let keys: Vec<u16> = all.iter().map(|e| e.0).collect();
    assert!(keys.windows(2).all(|w| w[0] < w[1]) && keys[0] <= 2 && keys[2047] <= 2050, "the nearest, by slot");
    // a quarter second on, nobody moved: one empty datagram
    k.clear();
    k.step(300.0);
    k.from(W(0), &mapreq_base(100, 2, 2048, 1));
    assert_eq!(mapds(&k).into_iter().map(|(_, x)| (x.0, x.1, x.2, x.3, x.5.len())).collect::<Vec<_>>(), [(2, 1, 0, 1, 0)]);
}

// roam_core asks as the hub reads it, and takes the hub's answer in parts, in any order
#[test]
fn the_world_map_on_the_wire_roam_core_asks_and_takes_the_answer() {
    let deps = repo().join("deps");
    let wasi = std::fs::read_dir(&deps).ok().and_then(|d| {
        let mut v: Vec<PathBuf> =
            d.flatten().filter(|e| e.file_name().to_string_lossy().starts_with("wasi-sdk-")).map(|e| e.path()).collect();
        v.sort();
        v.pop()
    });
    let Some(wasi) = wasi else {
        eprintln!("no wasi-sdk in deps/: skipped");
        return;
    };
    if Command::new("node").arg("--version").output().is_err() {
        eprintln!("no node: skipped");
        return;
    }
    let w = Path::new(env!("CARGO_TARGET_TMPDIR")).join("roam_hubwire_map.wasm");
    let out = Command::new(wasi.join("bin/clang"))
        .current_dir(repo())
        .args(["--target=wasm32-wasip1-threads", "-pthread", "-O2", "-ffp-contract=off", "-Iruntime", "-o"])
        .arg(&w)
        .args(["web/test/roam_hubwire.c", "runtime/roam_core.c"])
        .arg("-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=268435456")
        .output()
        .expect("clang");
    assert!(out.status.success(), "clang: {}", String::from_utf8_lossy(&out.stderr));
    let run = |args: &[String]| -> String {
        let o = Command::new("node").arg(repo().join("web/test/wasi-run.mjs")).arg(&w).args(args).output().expect("node");
        String::from_utf8_lossy(&o.stdout).trim().to_string()
    };
    let hex = |b: &[u8]| b.iter().map(|x| format!("{x:02x}")).collect::<String>();
    let unhex = |h: &str| -> Vec<u8> { (0..h.len() / 2).map(|i| u8::from_str_radix(&h[2 * i..2 * i + 2], 16).unwrap()).collect() };
    // the hub's answer to 300 players (three parts), the last first: the session asks with req 1, from nothing
    let mut k = Kit::new(RoamOptions { degree: 1, ..RoamOptions::default() });
    for i in 0..301 {
        join(&mut k, i);
        k.from(W(i), &snap_moving(100 + i, [i as f64 * -3.0, 7.0, 0.0], [0.0, -(i as f64 % 9.0), 0.0], -std::f64::consts::FRAC_PI_2, 2));
    }
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq_base(100, 1, 2048, 0));
    let mut first: Vec<String> = k.sent.lock().iter().map(|(_, f)| hex(&f[16..])).collect();
    assert_eq!(first.len(), 3);
    first.reverse();
    let got = run(&[vec!["map".to_string()], first.clone()].concat());
    let mut lines = got.lines();
    let ask = lines.next().unwrap_or("");
    assert_eq!(ask.strip_prefix("ask ").map(|h| h.len()), Some(2 * (16 + 6 + 4)), "{got}");
    // its MAPREQ, as the hub reads it: req 1, 2048 at most, from nothing
    let a = unhex(&ask[4..]);
    assert_eq!(
        (a[5], u16::from_le_bytes([a[16], a[17]]), u16::from_le_bytes([a[18], a[19]]), u16::from_le_bytes([a[20], a[21]])),
        (10, 1, 2048, 0)
    );
    let entry = |i: u32, x: f64, vy: i32| format!("{i}:{},7,192,0,{vy}", x as i32);
    let want: Vec<String> = (2..301).map(|i| entry(i, -3.0 * i as f64, -((i % 9) as i32))).collect();
    assert_eq!(lines.next().unwrap_or(""), format!("map 1 299 299 {}", want.join(" ")));
    // one part only: nothing yet
    let got = run(&["map".to_string(), first[0].clone()]);
    assert!(got.ends_with("map 0 0 0"), "{got}");
    // it asks again from answer 1 (req 2): 5 went, 6 moved off; the rest where it moved them on
    k.step(300.0);
    k.from(W(5), &bye(105));
    k.from(W(6), &snap_moving(106, [-18.0, 40.0, 0.0], [0.0; 3], -std::f64::consts::FRAC_PI_2, 2));
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq_base(100, 2, 2048, 1));
    let second: Vec<String> = k.sent.lock().iter().filter(|(_, f)| f[16 + 5] == 17).map(|(_, f)| hex(&f[16..])).collect();
    assert_eq!(second.len(), 1);
    assert_eq!(mapd_answer(&k), (2, 1, 298, vec![(5, None), (6, Some((-18, 40, 192, 0, 0)))]));
    let got = run(&[vec!["map".to_string()], first, vec!["ask".to_string()], second].concat());
    let mut lines = got.lines();
    let ask2 = unhex(&lines.nth(1).unwrap_or("").strip_prefix("ask ").unwrap_or("")[..]);
    assert_eq!((u16::from_le_bytes([ask2[16], ask2[17]]), u16::from_le_bytes([ask2[20], ask2[21]])), (2, 1), "it asks from answer 1");
    let want: Vec<String> =
        (2..301).filter(|&i| i != 5).map(|i| if i == 6 { "6:-18,40,192,0,0".to_string() } else { entry(i, -3.0 * i as f64, -((i % 9) as i32)) }).collect();
    assert_eq!(lines.next().unwrap_or(""), format!("map 2 298 298 {}", want.join(" ")));
}
