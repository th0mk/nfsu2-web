//! The minimap's answer (NEARREQ / NEARD, runtime/roam_game.c near_update): the players of the asker's room in free
//! roam within the radius it asks for, past itself and its links, the nearest it asks for, each where it is now, as
//! what changed since the answer it holds, in one datagram; one answer a browser every 150 ms, fewer while the hub's
//! thread is busy.

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

/// the NEARD datagrams the hub sent since the last clear
fn nears(k: &Kit) -> Vec<(u32, Mapd)> {
    k.sent.lock().iter().filter_map(|(vip, f)| neard_of(f).map(|m| (*vip, m))).collect()
}

/// the one NEARD answer sent since the last clear (one datagram: part 0 of 1): (req, base, total, entries)
fn near_answer(k: &Kit) -> (u16, u16, u16, Vec<MapdEntry>) {
    let m = nears(k);
    assert_eq!(m.len(), 1, "one datagram");
    let (_, (req, base, part, nparts, total, e)) = m.into_iter().next().unwrap();
    assert_eq!((part, nparts), (0, 1));
    (req, base, total, e)
}

// the players within the radius, past the one asking and its links; not those in a race, in another room, or farther;
// the nearest when it asks for fewer; a player alone gets one empty datagram, a stranger (no HELLO) nothing
#[test]
fn the_minimap_shows_the_players_within_its_radius() {
    let mut k = Kit::new(RoamOptions { degree: 1, ..RoamOptions::default() });
    for i in 0..10 {
        join(&mut k, i);
    }
    k.from(W(10), &hello_with(110, &HelloOpts { room: fnv1a("blue"), ..Default::default() }));
    // player i at x = 30 i, heading a quarter turn (64); 10 (another room) right beside the one asking
    for i in 0..=10u32 {
        let status = if i == 7 { 3 } else { 2 };
        let x = if i == 10 { 15.25 } else { i as f64 * 30.0 + 0.25 };
        k.from(W(i), &snap_moving(100 + i, [x, 3.75, 0.0], [0.0; 3], std::f64::consts::FRAC_PI_2, status));
    }
    k.hub.tick();
    k.clear();
    k.from(W(0), &nearreq(100, 1, 64, 0, 200));
    // its link is 1 (the nearest); 7 races; 8 and 9 are past 200 m; 10 is in another room
    let want: Vec<MapdEntry> = (2..7u16).map(|i| (i, Some((i as i16 * 30, 4, 64, 0, 0)))).collect();
    assert_eq!(near_answer(&k), (1, 0, 5, want));
    // the nearest two, for a page that asks for two (what it held goes)
    k.clear();
    k.step(200.0);
    k.from(W(0), &nearreq(100, 2, 2, 1, 200));
    let (req, base, _, e) = near_answer(&k);
    assert_eq!((req, base), (2, 1));
    assert_eq!(e, vec![(4, None), (5, None), (6, None)], "2 and 3 stay, the rest go");
    // a player alone in its room: one empty datagram; a stranger: nothing
    k.clear();
    k.from(W(10), &nearreq(110, 1, 64, 0, 200));
    assert_eq!(near_answer(&k), (1, 0, 0, vec![]));
    k.clear();
    k.from(W(20), &nearreq(120, 1, 64, 0, 200));
    assert!(nears(&k).is_empty());
}

// what changed since the answer the page holds: a car going straight stays where the page moves it on, one that
// turned 3 m off (past the minimap's 2 m) is placed again, one gone goes, one standing never comes again
#[test]
fn the_minimap_sends_what_changed() {
    let mut k = Kit::new(RoamOptions { degree: 1, ..RoamOptions::default() });
    for i in 0..6 {
        join(&mut k, i);
    }
    // 0 asks; 1 its link; 2 stands; 3 drives east at 20 m/s; 4 too, then turns; 5 is to go
    let at = |i: u32, t: f64| -> ([f64; 3], [f64; 3]) {
        match i {
            3 => ([60.0 + 20.0 * t, 0.0, 0.0], [20.0, 0.0, 0.0]),
            4 if t > 0.0 => ([80.0 + 20.0 * t, 3.0, 0.0], [20.0, 0.0, 0.0]),
            4 => ([80.0, 0.0, 0.0], [20.0, 0.0, 0.0]),
            _ => ([i as f64 * 20.0, 0.0, 0.0], [0.0; 3]),
        }
    };
    let snaps = |k: &mut Kit, t: f64| {
        for i in 0..6 {
            let (p, v) = at(i, t);
            k.from(W(i), &snap_moving(100 + i, p, v, 0.0, 2));
        }
    };
    snaps(&mut k, 0.0);
    k.hub.tick();
    k.clear();
    k.from(W(0), &nearreq(100, 1, 64, 0, 300));
    assert_eq!(
        near_answer(&k),
        (
            1,
            0,
            4,
            vec![(2, Some((40, 0, 0, 0, 0))), (3, Some((60, 0, 0, 20, 0))), (4, Some((80, 0, 0, 20, 0))), (5, Some((100, 0, 0, 0, 0)))]
        )
    );
    k.step(250.0);
    k.from(W(5), &bye(105));
    snaps(&mut k, 0.25);
    k.hub.tick();
    k.clear();
    k.from(W(0), &nearreq(100, 2, 64, 1, 300));
    assert_eq!(near_answer(&k), (2, 1, 3, vec![(4, Some((85, 3, 0, 20, 0))), (5, None)]));
    // the next question, without a tick between (the same grid, the candidates picked at the last): the same cars,
    // where they are now
    k.clear();
    k.step(150.0);
    k.from(W(0), &nearreq(100, 3, 64, 2, 300));
    assert_eq!(near_answer(&k), (3, 2, 3, vec![]));
}

// one answer a browser every 150 ms; while the hub's thread is busy, every 500 ms (60 %) or every second (75 %)
#[test]
fn the_minimap_is_answered_less_often_while_the_hubs_thread_is_busy() {
    let mut k = Kit::new(RoamOptions::default());
    for i in 0..10 {
        join(&mut k, i);
        k.from(W(i), &snap(100 + i, [i as f64 * 10.0, 0.0, 0.0]));
    }
    k.hub.tick();
    let mut req = 0u16;
    let mut ask = |k: &mut Kit, after: f64| -> bool {
        k.step(after);
        k.clear();
        req += 1;
        k.from(W(0), &nearreq(100, req, 64, 0, 300));
        !nears(k).is_empty()
    };
    assert!(ask(&mut k, 0.0));
    assert!(!ask(&mut k, 100.0), "within 150 ms");
    assert!(ask(&mut k, 60.0));
    k.hub.set_thread_busy(0.65);
    assert!(ask(&mut k, 200.0), "(the gap after the last answer was 150 ms)");
    assert!(!ask(&mut k, 300.0), "within 500 ms");
    assert!(ask(&mut k, 250.0));
    k.hub.set_thread_busy(0.8);
    assert!(ask(&mut k, 600.0));
    assert!(!ask(&mut k, 700.0), "within a second");
    assert!(ask(&mut k, 350.0));
    k.hub.set_thread_busy(0.1);
    assert!(ask(&mut k, 1000.0));
    assert!(ask(&mut k, 200.0));
}

// a crowd: the 128 nearest at most, each placed (1152 bytes, one datagram); then what changed, or from nothing again
// when that would not fit
#[test]
fn the_minimap_answers_a_crowd_in_one_datagram() {
    let mut k = Kit::new(RoamOptions::default());
    for i in 0..301 {
        join(&mut k, i);
        k.from(W(i), &snap(100 + i, [(i % 20) as f64 * 5.0 + 0.25, (i / 20) as f64 * 5.0 + 0.25, 0.0]));
    }
    k.hub.tick();
    k.clear();
    k.from(W(0), &nearreq(100, 1, 1000, 0, 1000));
    let (_, base, total, e) = near_answer(&k);
    assert_eq!((base, e.len(), e.iter().all(|x| x.1.is_some())), (0, 128, true));
    assert_eq!(total, 256, "the 256 candidates within the radius counted (past the 32 links)");
    let len = k.sent.lock()[0].1.len() - 16;
    assert_eq!(len, 16 + 9 + 128 * 9 + 4);
    // everyone moved 10 m: the same 128, each placed again (1152 bytes: it fits)
    k.step(300.0);
    for i in 0..301 {
        k.from(W(i), &snap(100 + i, [(i % 20) as f64 * 5.0 + 10.25, (i / 20) as f64 * 5.0 + 0.25, 0.0]));
    }
    k.hub.tick();
    k.clear();
    k.from(W(0), &nearreq(100, 2, 1000, 1, 1000));
    let (_, base, _, e) = near_answer(&k);
    assert_eq!((base, e.len(), e.iter().all(|x| x.1.is_some())), (1, 128, true), "from answer 1");
    // and again, the one asking 60 m on: some of its 128 go, the others moved: 128 placed and the gone ones would not
    // fit, so from nothing
    k.step(300.0);
    for i in 0..301 {
        let x = if i == 0 { 60.25 } else { (i % 20) as f64 * 5.0 + 20.25 };
        k.from(W(i), &snap(100 + i, [x, (i / 20) as f64 * 5.0 + 0.25, 0.0]));
    }
    k.hub.tick();
    k.clear();
    k.from(W(0), &nearreq(100, 3, 1000, 2, 1000));
    let (_, base, _, e) = near_answer(&k);
    assert_eq!((base, e.len(), e.iter().all(|x| x.1.is_some())), (0, 128, true), "from nothing");
}

// roam_core asks as the hub reads it, and takes the hub's answer (web/test/roam_hubwire.c)
#[test]
fn the_minimap_on_the_wire_roam_core_asks_and_takes_the_answer() {
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
    let w = Path::new(env!("CARGO_TARGET_TMPDIR")).join("roam_hubwire_near.wasm");
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
    let mut k = Kit::new(RoamOptions { degree: 1, ..RoamOptions::default() });
    for i in 0..6 {
        join(&mut k, i);
        k.from(W(i), &snap_moving(100 + i, [i as f64 * -40.0, 7.0, 0.0], [0.0, -(i as f64), 0.0], -std::f64::consts::FRAC_PI_2, 2));
    }
    k.hub.tick();
    k.clear();
    // its NEARREQ, as the hub reads it: type 18, req 1, 64 at most, from nothing, within 300 m
    let got = run(&["near".to_string()]);
    let ask = unhex(got.lines().next().unwrap_or("").strip_prefix("ask ").unwrap_or(""));
    assert_eq!(ask.len(), 16 + 8 + 4, "{got}");
    let u = |o: usize| u16::from_le_bytes([ask[o], ask[o + 1]]);
    assert_eq!((ask[5], u(16), u(18), u(20), u(22)), (18, 1, 64, 0, 300));
    // the same question as player 0's, and the session takes the answer
    k.from(W(0), &nearreq(100, 1, 64, 0, 300));
    let first: Vec<String> = k.sent.lock().iter().map(|(_, f)| hex(&f[16..])).collect();
    assert_eq!(first.len(), 1);
    let got = run(&[vec!["near".to_string()], first.clone()].concat());
    let entry = |i: u32| format!("{i}:{},7,192,0,{}", -40 * i as i32, -(i as i32));
    let want: Vec<String> = (2..6).map(entry).collect();
    assert_eq!(got.lines().nth(1).unwrap_or(""), format!("near 1 4 4 {}", want.join(" ")));
    // it asks again from answer 1: 3 went, 4 moved off; the rest where it moved them on
    k.step(300.0);
    k.from(W(3), &bye(103));
    k.from(W(4), &snap_moving(104, [-160.0, 40.0, 0.0], [0.0; 3], -std::f64::consts::FRAC_PI_2, 2));
    k.hub.tick();
    k.clear();
    k.from(W(0), &nearreq(100, 2, 64, 1, 300));
    let second: Vec<String> = k.sent.lock().iter().map(|(_, f)| hex(&f[16..])).collect();
    assert_eq!(near_answer(&k), (2, 1, 3, vec![(3, None), (4, Some((-160, 40, 192, 0, 0)))]));
    let got = run(&[vec!["near".to_string()], first, vec!["ask".to_string()], second].concat());
    let mut lines = got.lines();
    let ask2 = unhex(lines.nth(1).unwrap_or("").strip_prefix("ask ").unwrap_or(""));
    assert_eq!((u16::from_le_bytes([ask2[16], ask2[17]]), u16::from_le_bytes([ask2[20], ask2[21]])), (2, 1), "it asks from answer 1");
    let want: Vec<String> = [2, 4, 5].iter().map(|&i| if i == 4 { "4:-160,40,192,0,0".to_string() } else { entry(i) }).collect();
    assert_eq!(lines.next().unwrap_or(""), format!("near 2 3 3 {}", want.join(" ")));
}
