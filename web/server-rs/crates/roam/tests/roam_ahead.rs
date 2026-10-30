//! What the hub does that the Node server's hub never did: links that look ahead at the cars' speeds, and the world
//! map's answer (MAPREQ / MAP).

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
            let status = if i % 7 == 0 {
                4
            } else if i % 11 == 0 {
                5
            } else {
                2
            };
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

/// the MAP datagrams the hub sent since the last clear
fn maps(k: &Kit) -> Vec<(u32, Map)> {
    k.sent.lock().iter().filter_map(|(vip, f)| map_of(f).map(|m| (*vip, m))).collect()
}

// the world map: the players of the room in free roam, past the one asking and its links, nearest first, in parts of
// 230, 512 at most to a page that asks for no more; not those in a race, another room or gone quiet; one answer a second
#[test]
fn the_world_map_lists_the_others_nearest_first() {
    let mut k = Kit::new(RoamOptions { degree: 1, ..RoamOptions::default() });
    for i in 0..600 {
        join(&mut k, i);
    }
    k.from(W(600), &hello_with(700, &HelloOpts { core: 4, room: fnv1a("blue"), ..Default::default() }));
    // player i at x = 10 i (rounded), heading a quarter turn (64)
    for i in 0..=600u32 {
        let status = if i == 7 { 3 } else { 2 };
        k.from(W(i), &snap_moving(100 + i, [i as f64 * 10.0 + 0.25, 3.75, 0.0], [0.0; 3], std::f64::consts::FRAC_PI_2, status));
    }
    k.from(W(600), &snap(700, [5.0, 0.0, 0.0]));
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq(100, 9));
    let m = maps(&k);
    assert_eq!(m.iter().map(|(_, x)| (x.0, x.1, x.2, x.4.len())).collect::<Vec<_>>(), [(9, 0, 3, 230), (9, 1, 3, 230), (9, 2, 3, 52)]);
    assert!(m.iter().all(|(vip, x)| *vip == W(0) && x.3 == 597), "to the one asking: 600 less itself, its link, the one racing");
    let all: Vec<(i16, i16, u8)> = m.into_iter().flat_map(|(_, x)| x.4).collect();
    // its link is 1 (the nearest), 7 races, 600 is in another room
    let want: Vec<(i16, i16, u8)> = (2..600).filter(|&i| i != 7).take(512).map(|i| (i * 10, 4, 64)).collect();
    assert_eq!(all, want);
    // again within a second: nothing; after it: an answer
    k.clear();
    k.step(500.0);
    k.from(W(0), &mapreq(100, 10));
    assert!(maps(&k).is_empty(), "one answer a second");
    k.step(600.0);
    k.from(W(0), &mapreq(100, 11));
    assert_eq!(maps(&k).len(), 3);
    // a player alone in its room: one empty part; a stranger (no HELLO): nothing
    k.clear();
    k.from(W(600), &mapreq(700, 1));
    assert_eq!(maps(&k).into_iter().map(|(_, x)| x).collect::<Vec<_>>(), [(1, 0, 1, 0, vec![])]);
    k.clear();
    k.from(W(900), &mapreq(900, 1));
    assert!(maps(&k).is_empty());
    // positions older than 5 s are not shown
    k.step(5000.0);
    k.from(W(1), &snap(101, [10.0, 0.0, 0.0]));
    k.clear();
    k.from(W(0), &mapreq(100, 12));
    assert_eq!(maps(&k).into_iter().map(|(_, x)| (x.3, x.4.len())).collect::<Vec<_>>(), [(0, 0)], "only its link 1 is fresh");
}

// a page that says how many it takes: 2048 at most, in nine parts (the core takes them in any order), fewer when it
// asks for fewer
#[test]
fn the_world_map_takes_2048_from_a_page_that_asks() {
    let mut k = Kit::new(RoamOptions { degree: 1, ..RoamOptions::default() });
    for i in 0..2200 {
        join(&mut k, i);
        k.from(W(i), &snap(100 + i, [i as f64 * 5.0, 0.0, 0.0]));
    }
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq_max(100, 1, 4000));
    let m = maps(&k);
    let parts: Vec<(u8, u8, usize)> = m.iter().map(|(_, x)| (x.1, x.2, x.4.len())).collect();
    assert_eq!(parts.len(), 9);
    assert!(parts.iter().enumerate().all(|(j, p)| p.0 as usize == j && p.1 == 9), "{parts:?}");
    assert_eq!(parts.iter().map(|p| p.2).sum::<usize>(), 2048);
    assert!(m.iter().all(|(_, x)| x.3 == 2198), "2200 less itself and its link");
    let all: Vec<i16> = m.into_iter().flat_map(|(_, x)| x.4).map(|e| e.0).collect();
    assert!(all.windows(2).all(|w| w[0] < w[1]) && all[0] <= 10 && all[2047] <= 2050 * 5, "nearest first");
    k.clear();
    k.step(1100.0);
    k.from(W(0), &mapreq_max(100, 2, 300));
    assert_eq!(maps(&k).into_iter().map(|(_, x)| (x.1, x.2, x.4.len())).collect::<Vec<_>>(), [(0, 2, 230), (1, 2, 70)]);
}

// one answer a second for a browser, whatever its endpoints: a BYE and a HELLO, or another port, make a new player,
// not a new answer (the second security audit: that way a few clients kept the hub's thread busy with maps)
#[test]
fn the_world_map_answers_a_browser_once_a_second_whatever_its_endpoints() {
    let mut k = Kit::new(RoamOptions::default());
    for i in 0..20 {
        join(&mut k, i);
        k.from(W(i), &snap(100 + i, [i as f64 * 10.0, 0.0, 0.0]));
    }
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq(100, 1));
    assert_eq!(maps(&k).len(), 1);
    // again as a new player on the same endpoint, then on other ports: nothing within the second
    k.clear();
    k.from(W(0), &bye(100));
    join(&mut k, 0);
    k.from(W(0), &mapreq(100, 2));
    for port in [47211, 47212] {
        k.hub.frame(W(0), &frame_ports(W(0), &hello_with(100, &HelloOpts { core: 4, ..Default::default() }), port, 47210));
        k.hub.frame(W(0), &frame_ports(W(0), &mapreq(100, 3), port, 47210));
    }
    assert!(maps(&k).is_empty(), "one answer a second for the browser");
    // another browser is not held back by it; and past the second (and a tick, which forgets old answers) it asks again
    k.from(W(1), &mapreq(101, 1));
    assert_eq!(maps(&k).len(), 1);
    k.clear();
    k.step(1000.0);
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq(100, 4));
    assert_eq!(maps(&k).len(), 1);
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
    // the hub's answer to 300 players (two parts): the session asks with req 1
    let mut k = Kit::new(RoamOptions { degree: 1, ..RoamOptions::default() });
    for i in 0..301 {
        join(&mut k, i);
        k.from(W(i), &snap_moving(100 + i, [i as f64 * -3.0, 7.0, 0.0], [0.0; 3], -std::f64::consts::FRAC_PI_2, 2));
    }
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq(100, 1));
    let frames: Vec<String> = k.sent.lock().iter().map(|(_, f)| hex(&f[16..])).collect();
    assert_eq!(frames.len(), 2);
    let got = run(&["map".to_string(), frames[1].clone(), frames[0].clone()]);
    let mut lines = got.lines();
    let ask = lines.next().unwrap_or("");
    assert_eq!(ask.strip_prefix("ask ").map(|h| h.len()), Some(2 * (16 + 4 + 4)), "{got}");
    // its MAPREQ, as the hub reads it: req 1, 2048 at most
    let a: Vec<u8> = (0..ask.len() / 2 - 2).map(|i| u8::from_str_radix(&ask[4 + 2 * i..6 + 2 * i], 16).unwrap()).collect();
    assert_eq!((a[5], u16::from_le_bytes([a[16], a[17]]), u16::from_le_bytes([a[18], a[19]])), (10, 1, 2048));
    let want: Vec<String> = (2..301).map(|i| format!("{},7,192", -3 * i)).collect();
    assert_eq!(lines.next().unwrap_or(""), format!("map 1 299 299 {}", want.join(" ")));
    // one part only: nothing yet
    let got = run(&["map".to_string(), frames[0].clone()]);
    assert!(got.ends_with("map 0 0 0"), "{got}");
    // 2048 of 2200 in nine parts, the last first
    let mut k = Kit::new(RoamOptions { degree: 1, ..RoamOptions::default() });
    for i in 0..2200 {
        join(&mut k, i);
        k.from(W(i), &snap_moving(100 + i, [i as f64 * 5.0, -2.0, 0.0], [0.0; 3], 0.0, 2));
    }
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq_max(100, 1, 2048));
    let want: Vec<String> = maps(&k).into_iter().flat_map(|(_, x)| x.4).map(|e| format!("{},{},{}", e.0, e.1, e.2)).collect();
    let mut frames: Vec<String> = k.sent.lock().iter().map(|(_, f)| hex(&f[16..])).collect();
    assert_eq!((frames.len(), want.len()), (9, 2048));
    frames.reverse();
    let got = run(&[vec!["map".to_string()], frames].concat());
    assert_eq!(got.lines().nth(1).unwrap_or(""), format!("map 1 2198 2048 {}", want.join(" ")));
}
