//! The cheat watch (watch.rs) through the hub: a teleport onto another player is a `[cheat]` line that names it, the
//! game's own jumps are none, and a player who leaves after a cheat gets its summary.

mod kit;

use kit::*;
use nfsu2_roam::RoamOptions;

/// a player of `room` named `name`, in status `status`
fn hello(k: &mut Kit, i: u32, name: &[u8], status: u8) {
    k.from(V(i), &hello_with(100 + i, &HelloOpts { core: 5, room: fnv1a("bay"), name, status, ..Default::default() }));
}

/// Ann (V(0)) parked at (1000, 0, 0) and Bob (V(1)) at the origin, each with a SNAP 500 ms ago
fn two() -> Kit {
    let mut k = Kit::new(RoamOptions::default());
    hello(&mut k, 0, b"Ann", 2);
    hello(&mut k, 1, b"Bob", 2);
    k.from(V(0), &snap(100, [1000.0, 0.0, 0.0]));
    k.from(V(1), &snap(101, [0.0, 0.0, 0.0]));
    k.step(500.0);
    k.from(V(0), &snap(100, [1000.0, 0.0, 0.0]));
    k
}

fn cheats(k: &Kit) -> Vec<String> {
    k.lines.lock().iter().filter(|l| l.starts_with("[cheat]")).cloned().collect()
}

// Bob teleports 1 km onto Ann: a line with his address and hers; his BYE sums it up
#[test]
fn a_teleport_onto_a_player_names_both() {
    let mut k = two();
    k.from(V(1), &snap(101, [1005.0, 0.0, 0.0]));
    assert_eq!(
        cheats(&k),
        ["[cheat] 10.77.0.11 \"Bob\": teleport 1: 1.0 km in 0.5 s, 918 m more than a car drives, (0, 0, 0) to (1005, 0, 0); \
          next to \"Ann\" 10.77.0.10 (5 m)"]
    );
    k.lines.lock().clear();
    k.step(500.0);
    k.from(V(1), &bye(101));
    assert_eq!(cheats(&k), ["[cheat] 10.77.0.11 \"Bob\", 1 s in free roam (left): 1 teleport (1 next to other players)"]);
}

// at a race lobby's start: the line says whose
#[test]
fn a_teleport_to_a_race_lobby_says_so() {
    let mut k = two();
    let mut p = vec![0u8; 28];
    p[0] = 1; // advert
    p[3] = 4;
    p[8..12].copy_from_slice(&0xA1u32.to_le_bytes());
    p[12..16].copy_from_slice(&0x0BADF00Du32.to_le_bytes());
    p[16..18].copy_from_slice(&1000i16.to_le_bytes());
    p[22] = 1;
    p[24..28].copy_from_slice(&100u32.to_le_bytes());
    k.from(V(0), &dgram(12, 100, &p));
    k.from(V(1), &snap(101, [1010.0, 10.0, 0.0]));
    let l = cheats(&k);
    assert!(l.len() == 1 && l[0].ends_with("; at the start of \"Ann\"'s race lobby"), "{l:?}");
}

// no line for the game's own jumps: a Respawn (250 m), Jump to Garage (a status out of free roam between), a long
// drive at 100 m/s; and nobody is named who is not next to the car
#[test]
fn the_games_own_jumps_are_none() {
    let mut k = two();
    k.from(V(1), &snap(101, [250.0, 0.0, 0.0]));
    k.step(500.0);
    hello(&mut k, 1, b"Bob", 0);
    k.step(3000.0);
    hello(&mut k, 1, b"Bob", 2);
    k.from(V(1), &snap(101, [-3000.0, 2000.0, 0.0]));
    for i in 1..=300 {
        k.step(500.0);
        k.from(V(1), &snap(101, [-3000.0 + 50.0 * i as f64, 2000.0, 0.0]));
    }
    assert_eq!(cheats(&k), Vec::<String>::new());
    k.step(500.0);
    k.from(V(1), &snap(101, [-5000.0, 2000.0, 0.0]));
    let l = cheats(&k);
    assert!(l.len() == 1 && l[0].ends_with("(12000, 2000, 0) to (-5000, 2000, 0)"), "{l:?}");
    k.hub.tick();
    assert_eq!(k.info("cheatLines"), 1.0);
}

// off the switch and back within a minute (a bad connection's page): Bob's watch goes on (the teleport across the
// gap is his second, the summary one, at his BYE); gone for good, his summary comes a minute later, as of when he went
#[test]
fn a_watch_outlives_a_dropped_connection() {
    let mut k = two();
    k.from(V(1), &snap(101, [1005.0, 0.0, 0.0]));
    k.hub.peer_gone(V(1));
    k.step(20_000.0);
    k.hub.tick();
    assert_eq!(cheats(&k).len(), 1, "no summary yet: {:?}", cheats(&k));
    hello(&mut k, 1, b"Bob", 2);
    k.from(V(1), &snap(101, [9000.0, 0.0, 0.0]));
    let l = cheats(&k);
    assert!(l.len() == 2 && l[1].starts_with("[cheat] 10.77.0.11 \"Bob\": teleport 2: 8.0 km in 20.0 s"), "{l:?}");
    k.from(V(1), &bye(101));
    assert_eq!(cheats(&k)[2], "[cheat] 10.77.0.11 \"Bob\", 20 s in free roam (left): 2 teleports (1 next to other players)");
    // Ann (silent since, gone from the hub, her watch kept), back with a teleport of hers, then gone for good
    k.lines.lock().clear();
    hello(&mut k, 0, b"Ann", 2);
    k.from(V(0), &snap(100, [-5000.0, 0.0, 0.0]));
    k.hub.peer_gone(V(0));
    k.step(30_000.0);
    k.hub.tick();
    assert_eq!(cheats(&k).len(), 1, "her teleport, no summary yet");
    k.step(31_000.0);
    k.hub.tick();
    assert_eq!(cheats(&k)[1], "[cheat] 10.77.0.10 \"Ann\", 20 s in free roam (left): 1 teleport");
    assert_eq!(k.info("watchesKept"), 0.0);
}

// another start of the game (a new id) on that endpoint: where its car was counts for nothing
#[test]
fn another_start_of_the_game_starts_its_watch_afresh() {
    let mut k = two();
    k.hub.peer_gone(V(1));
    k.step(5000.0);
    k.from(V(1), &hello_with(999, &HelloOpts { core: 5, room: fnv1a("bay"), name: b"Bob", status: 2, ..Default::default() }));
    k.from(V(1), &snap(999, [9000.0, 0.0, 0.0]));
    assert_eq!(cheats(&k), Vec::<String>::new());
}

/// a RACERES datagram as roam_core.c lays it out: lobby, race, mode, laps, n, 0; rows (id, flags, 3 bytes 0, time,
/// done, dist)
fn raceres(sender: u32, lobby: u32, rows: &[(u32, u8, f32, f32, f32)]) -> Vec<u8> {
    let mut p = vec![0u8; 12 + 20 * rows.len()];
    p[0..4].copy_from_slice(&lobby.to_le_bytes());
    p[4..8].copy_from_slice(&0x7C80_E775u32.to_le_bytes());
    p[8] = 1;
    p[9] = 1;
    p[10] = rows.len() as u8;
    for (k, &(id, flags, time, done, dist)) in rows.iter().enumerate() {
        let o = 12 + 20 * k;
        p[o..o + 4].copy_from_slice(&id.to_le_bytes());
        p[o + 4] = flags;
        p[o + 8..o + 12].copy_from_slice(&time.to_le_bytes());
        p[o + 12..o + 16].copy_from_slice(&done.to_le_bytes());
        p[o + 16..o + 20].copy_from_slice(&dist.to_le_bytes());
    }
    dgram(16, sender, &p)
}

// a race the game ended at once for Bob (a test's racewin, its real rows): his win is a [cheat] line once both reports
// came, naming him by his address though he left first
#[test]
fn a_race_won_by_cheating_names_its_winner() {
    let mut k = two();
    // (rows: Bob 101 first, Ann 100; flags 2: the reporter's own)
    k.from(V(1), &raceres(101, 0xA1, &[(101, 2, 4.75, 0.006, 38.0), (100, 0, 34.75, 0.168, 901.0)]));
    k.from(V(1), &bye(101));
    k.lines.lock().clear();
    k.hub.tick();
    assert_eq!(cheats(&k), Vec::<String>::new(), "one of two reports");
    k.from(V(0), &raceres(100, 0xA1, &[(101, 0, 4.75, 0.006, 38.0), (100, 2, 34.75, 0.168, 901.0)]));
    k.hub.tick();
    let l = cheats(&k);
    assert!(
        l.len() == 1
            && l[0].starts_with("[cheat] 10.77.0.11 \"Bob\": won the race of lobby 000000a1 (race 7c80e775: sprint, 1 lap) in 0:04.8, ")
            && l[0].contains("which no car does, having driven 0.6 % of the way")
            && l[0].ends_with(": as 2 of 2 players' games saw it"),
        "{l:?}"
    );
}

// an honest race (a test's real rows): no line
#[test]
fn an_honest_race_is_none() {
    let mut k = two();
    // (flags 1 finished, 2 the reporter's own)
    let own = |me: u32, id: u32| 1 | if me == id { 2 } else { 0 };
    let rows = |me: u32| [(101, own(me, 101), 176.49, 1.0, 5265.0), (100, own(me, 100), 178.15, 1.0, 5281.0)];
    k.from(V(1), &raceres(101, 0xA2, &rows(101)));
    k.from(V(0), &raceres(100, 0xA2, &rows(100)));
    k.hub.tick();
    assert_eq!(cheats(&k), Vec::<String>::new());
}
