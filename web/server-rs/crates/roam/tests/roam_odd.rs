//! A car in free roam that moves as no car can (issue #8, "players flying on map"): the hub compares each SNAP in free
//! roam with the one before by the sender's own clock, and leaves a browser that made six such moves in ten seconds out
//! of free roam for ten minutes.
//! 1. a teleporter and a flyer: left out after their sixth odd move, linked with nobody, an empty list of their own,
//!    not on the world map, one console line each; a reload does not end it, ten minutes do;
//! 2. what a real car does is not odd: a fast drive, respawns, a jump, SNAPs that come at once after a stall, a clock
//!    that starts over, a shop between two places.

mod kit;

use kit::*;
use nfsu2_roam::RoamOptions;

const HALF_S: u32 = 500_000;

fn hello_named(sender: u32, name: &[u8]) -> Vec<u8> {
    hello_with(sender, &HelloOpts { name, core: 2, ..Default::default() })
}

fn has(k: &Kit, i: u32, j: u32) -> bool {
    k.lists().get(&format!("{}:47210", V(i))).is_some_and(|l| l.iter().any(|e| e.vip == V(j)))
}

fn list_len(k: &Kit, i: u32) -> usize {
    k.lists().get(&format!("{}:47210", V(i))).map_or(0, Vec::len)
}

fn left_out_lines(k: &Kit) -> Vec<String> {
    k.lines.lock().iter().filter(|l| l.contains("left out of free roam")).cloned().collect()
}

// a teleporter and a flyer: left out after their sixth odd move, linked with nobody, an empty list of their own, not
// on the world map, one console line each; a reload does not end it, ten minutes do
#[test]
fn a_teleporter_and_a_flyer_are_left_out_after_six_odd_moves_for_ten_minutes() {
    let mut k = Kit::new(RoamOptions::default());
    // 0 and 3 drive slowly near the start; 1 jumps 400 m back and forth every half second (800 m/s); 2 rises 15 m
    // every half second (30 m/s up) while it creeps forward
    for (i, name) in [(0, &b"anna"[..]), (1, b"tele"), (2, b"flyer"), (3, b"bob")] {
        k.from(V(i), &hello_named(100 + i, name));
    }
    let at = |i: u32, n: u32| -> [f64; 3] {
        match i {
            1 => [if n.is_multiple_of(2) { 10.0 } else { 410.0 }, 0.0, 0.0],
            2 => [20.0 + n as f64, 0.0, 15.0 * n as f64],
            _ => [i as f64 * 10.0 + n as f64, 5.0, 0.0],
        }
    };
    let send = |k: &mut Kit, n: u32| {
        for i in 0..4 {
            k.from(V(i), &snap_at(100 + i, at(i, n), n * HALF_S, 2, 0, 0));
        }
    };
    // the first SNAP and five odd moves: still in
    for n in 0..6 {
        send(&mut k, n);
        k.step(500.0);
    }
    k.hub.tick();
    assert!(left_out_lines(&k).is_empty(), "five odd moves: nobody left out yet");
    assert!(has(&k, 0, 2) && has(&k, 2, 0), "the flyer is still linked after five");
    // the sixth
    k.clear();
    send(&mut k, 6);
    k.step(500.0);
    k.hub.tick();
    let lines = left_out_lines(&k);
    assert_eq!(lines.len(), 2, "one line each: {lines:?}");
    assert!(lines.iter().any(|l| l.contains("\"tele\" 10.77.0.11")), "{lines:?}");
    assert!(lines.iter().any(|l| l.contains("\"flyer\" 10.77.0.12")), "{lines:?}");
    for cheat in [1, 2] {
        for i in [0, 3] {
            assert!(!has(&k, i, cheat) && !has(&k, cheat, i), "{cheat} and {i}: not linked");
        }
        assert_eq!(list_len(&k, cheat), 0, "{cheat}: an empty list, it drives alone");
    }
    assert!(has(&k, 0, 3), "the others still see each other");
    assert_eq!(k.info("leftOut"), 2.0);
    assert_eq!(k.info("out"), 2.0);
    // more odd moves: no more lines
    for n in 7..20 {
        send(&mut k, n);
        k.step(500.0);
    }
    k.hub.tick();
    assert_eq!(left_out_lines(&k).len(), 2, "said once");
    // the world map: anna's shows bob (linked: not on it) and neither of the two left out
    k.clear();
    k.from(V(0), &mapreq(100, 7));
    let map: Vec<_> = k.sent.lock().iter().filter_map(|(_, f)| map_of(f)).flat_map(|m| m.4).collect();
    assert!(map.is_empty(), "no one left out on the world map: {map:?}");
    // a reload: a new player at the same address, driving as a car does, is still out
    k.hub.peer_gone(V(1));
    k.from(V(1), &hello_named(201, b"tele"));
    for n in 0..4 {
        k.from(V(1), &snap_at(201, [12.0 + n as f64, 5.0, 0.0], n * HALF_S, 2, 0, 0));
        send_others(&mut k, 30 + n);
        k.step(500.0);
    }
    k.clear();
    k.hub.tick();
    assert!(!has(&k, 0, 1) && list_len(&k, 1) == 0, "a reload does not end it");
    // ten minutes on, driving as a car does: back
    k.step(600_000.0);
    for (i, s) in [(0, 100), (1, 201), (2, 102), (3, 103)] {
        k.from(V(i), &hello_named(s, b"p"));
        k.from(V(i), &snap_at(s, [i as f64 * 10.0, 5.0, 0.0], 3_000_000_000, 2, 0, 0));
    }
    k.clear();
    k.hub.tick();
    assert!(has(&k, 0, 1) && has(&k, 0, 2), "back after ten minutes");
    assert_eq!(k.info("out"), 0.0);
}

/// players 0, 2 and 3 of the first test on (the reload part): slow and near the start
fn send_others(k: &mut Kit, n: u32) {
    for i in [0u32, 2, 3] {
        k.from(V(i), &snap_at(100 + i, [i as f64 * 10.0 + 1.0, 5.0, 0.0], n * HALF_S, 2, 0, 0));
    }
}

// what a real car does is not odd: a fast drive, respawns, a jump, SNAPs that come at once after a stall, a clock
// that starts over, a shop between two places
#[test]
fn a_fast_drive_respawns_a_jump_a_stall_a_new_clock_and_a_shop_are_not_odd() {
    let mut k = Kit::new(RoamOptions::default());
    k.from(V(0), &hello_named(100, b"fast"));
    k.from(V(1), &hello_named(101, b"near"));
    let (mut x, mut z, mut t, mut clock) = (0.0f64, 0.0f64, 0u32, 0u8);
    let send = |k: &mut Kit, x: f64, z: f64, t: u32, status: u8, clock: u8, step_ms: f64| {
        k.from(V(0), &snap_at(100, [x, 0.0, z], t, status, 0, clock));
        k.from(V(1), &snap_at(101, [x + 15.0, 5.0, 0.0], t, 2, 0, 0));
        k.step(step_ms);
    };
    for n in 0..60u32 {
        t = t.wrapping_add(HALF_S);
        // 100 m/s (360 km/h) along x
        x += 50.0;
        match n {
            // a respawn 300 m on, twice in ten seconds
            10 | 18 => x += 300.0,
            // a jump off a ramp: 22 m/s then 17 up, then down again
            14 => z += 11.0,
            15 => z += 8.5,
            16 => z -= 6.0,
            17 => z -= 13.5,
            // the sender's clock starts over (a new clock byte), 2 km off: not compared
            40 => {
                clock = clock.wrapping_add(1);
                t = 77;
                x += 2000.0;
            }
            _ => {}
        }
        // a stall: SNAPs 25 to 29 come 60 ms apart, each half a second on by the sender's clock (by the hub's clock
        // they would be 830 m/s)
        let step = if (25..30).contains(&n) { 60.0 } else { 500.0 };
        // a shop at 50: status 4, then back 3 km off
        if n == 50 {
            send(&mut k, x, z, t, 4, clock, step);
            x += 3000.0;
            continue;
        }
        send(&mut k, x, z, t, 2, clock, step);
    }
    k.hub.tick();
    assert!(left_out_lines(&k).is_empty(), "{:?}", left_out_lines(&k));
    assert_eq!(k.info("leftOut"), 0.0);
    assert!(has(&k, 0, 1), "still linked");
}
