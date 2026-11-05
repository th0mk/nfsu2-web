//! The cheat watch (watch.rs) through the hub: a teleport onto another player is a `[cheat]` line that names it, the
//! game's own jumps are none, and a player who leaves after a cheat gets its summary.  The races' results (results.rs):
//! a race won by cheating is a line, its racers named as the endpoints that joined its lobby, its clean wins by two
//! clients.

mod kit;

use std::sync::Arc;

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

/// a RACERES datagram as roam_core.c lays it out: lobby, race, mode, laps, n, the heat; rows (id, flags, the jump, a
/// byte 0, time, done, dist)
fn raceres(sender: u32, lobby: u32, rows: &[(u32, u8, f32, f32, f32)]) -> Vec<u8> {
    raceres_heat(sender, lobby, 0, 0, rows, &[])
}

/// ...of heat `heat` of `heats` (the head's last byte heats << 4 | heat; 0 and 0 a race of one heat), each row's car's
/// jump (m, u16 after its flags)
fn raceres_heat(sender: u32, lobby: u32, heat: u8, heats: u8, rows: &[(u32, u8, f32, f32, f32)], jumps: &[u16]) -> Vec<u8> {
    let mut p = vec![0u8; 12 + 20 * rows.len()];
    p[0..4].copy_from_slice(&lobby.to_le_bytes());
    p[4..8].copy_from_slice(&0x7C80_E775u32.to_le_bytes());
    p[8] = 1;
    p[9] = 1;
    p[10] = rows.len() as u8;
    p[11] = heats << 4 | heat;
    for (k, &(id, flags, time, done, dist)) in rows.iter().enumerate() {
        let o = 12 + 20 * k;
        p[o..o + 4].copy_from_slice(&id.to_le_bytes());
        p[o + 4] = flags;
        p[o + 5..o + 7].copy_from_slice(&jumps.get(k).copied().unwrap_or(0).to_le_bytes());
        p[o + 8..o + 12].copy_from_slice(&time.to_le_bytes());
        p[o + 12..o + 16].copy_from_slice(&done.to_le_bytes());
        p[o + 16..o + 20].copy_from_slice(&dist.to_le_bytes());
    }
    dgram(16, sender, &p)
}

/// a LOBBY datagram of `sender`'s to the hub: `op` (1 advert, 2 an ask to join) of lobby `lobby`, race 0x7C80E775,
/// with the host (100) in it
fn lobby(sender: u32, op: u8, lobby: u32) -> Vec<u8> {
    let mut p = vec![0u8; 28];
    p[0] = op;
    p[3] = 4;
    p[8..12].copy_from_slice(&lobby.to_le_bytes());
    p[12..16].copy_from_slice(&0x7C80_E775u32.to_le_bytes());
    p[22] = 1;
    p[24..28].copy_from_slice(&100u32.to_le_bytes());
    dgram(12, sender, &p)
}

/// Ann's (100) lobby `l`, and Bob's (101) ask to join it
fn lobby_of_two(k: &mut Kit, l: u32) {
    k.from(V(0), &lobby(100, 1, l));
    k.from(V(1), &lobby(101, 2, l));
}

// a race the game ended at once for Bob (a test's racewin, its real rows): his win is a [cheat] line once both reports
// came, naming him by his address though he left first
#[test]
fn a_race_won_by_cheating_names_its_winner() {
    let mut k = two();
    lobby_of_two(&mut k, 0xA1);
    // (rows: Bob 101 first, Ann 100; flags 2: the reporter's own)
    k.from(V(1), &raceres(101, 0xA1, &[(101, 2, 4.75, 0.006, 38.0), (100, 0, 34.75, 0.168, 901.0)]));
    k.from(V(1), &bye(101));
    k.lines.lock().clear();
    k.hub.tick();
    assert_eq!(cheats(&k), Vec::<String>::new(), "one of two reports");
    k.from(V(0), &raceres(100, 0xA1, &[(101, 0, 4.75, 0.006, 38.0), (100, 2, 34.75, 0.168, 901.0)]));
    k.hub.tick();
    assert_eq!(
        cheats(&k),
        ["[cheat] 10.77.0.11 \"Bob\": won the race of lobby 000000a1 (race 7c80e775: sprint, 1 lap) in 0:04.8, having \
          driven 0.6 % of the way; nobody else finished (the race ends 30 s after the first): as 2 of 2 players' games saw it"]
    );
}

// an honest race (a test's real rows): no line
#[test]
fn an_honest_race_is_none() {
    let mut k = two();
    lobby_of_two(&mut k, 0xA2);
    // (flags 1 finished, 2 the reporter's own)
    let own = |me: u32, id: u32| 1 | if me == id { 2 } else { 0 };
    let rows = |me: u32| [(101, own(me, 101), 176.49, 1.0, 5265.0), (100, own(me, 100), 178.15, 1.0, 5281.0)];
    k.from(V(1), &raceres(101, 0xA2, &rows(101)));
    k.from(V(0), &raceres(100, 0xA2, &rows(100)));
    k.hub.tick();
    assert_eq!(cheats(&k), Vec::<String>::new());
}

// results name nobody who was not in the race: Bob's of a lobby the hub never saw, or of Ann's lobby before he asked to
// join it (Ann won in 4.75 s at 0.6 % of the way, they say), count for nothing; in it, a row naming Cat (who never
// joined) is nobody; and one results message of his a lobby, none within 20 s of his last
#[test]
fn results_name_only_the_lobbys_players() {
    let mut k = two();
    // (91 s on, every race judged; the three back on the hub)
    let later = |k: &mut Kit| {
        k.step(91_000.0);
        k.hub.tick();
        for (i, name) in [(0, &b"Ann"[..]), (1, b"Bob"), (2, b"Cat")] {
            hello(k, i, name, 2);
        }
    };
    later(&mut k);
    // (won in 4.75 s at 0.6 % of the way, by every report)
    let framed = |me: u32, who: u32| [(who, 0, 4.75, 0.006, 38.0), (me, 2, 34.75, 0.168, 901.0)];
    k.from(V(1), &raceres(101, 0xB1, &framed(101, 100)));
    later(&mut k);
    k.from(V(0), &lobby(100, 1, 0xB2));
    k.from(V(1), &raceres(101, 0xB2, &framed(101, 100)));
    later(&mut k);
    assert_eq!(cheats(&k), Vec::<String>::new(), "no lobby, or not in it");
    lobby_of_two(&mut k, 0xB3);
    k.from(V(1), &raceres(101, 0xB3, &framed(101, 102)));
    k.from(V(0), &raceres(100, 0xB3, &framed(100, 102)));
    later(&mut k);
    assert_eq!(cheats(&k), Vec::<String>::new(), "Cat is nobody");
    // Bob's real race's results, then fakes of the same and of another lobby of Ann's he is in
    lobby_of_two(&mut k, 0xB4);
    lobby_of_two(&mut k, 0xB5);
    let own = |me: u32, id: u32| 1 | if me == id { 2 } else { 0 };
    let honest = |me: u32| [(100, own(me, 100), 180.0, 1.0, 5200.0), (101, own(me, 101), 185.0, 1.0, 5300.0)];
    let fake = [(100, 1, 4.0, 1.0, 5200.0), (101, 3, 185.0, 1.0, 5300.0)];
    k.from(V(1), &raceres(101, 0xB4, &honest(101)));
    k.from(V(1), &raceres(101, 0xB5, &fake));
    k.from(V(0), &raceres(100, 0xB4, &honest(100)));
    later(&mut k);
    assert_eq!(cheats(&k), Vec::<String>::new(), "one results message a lobby, 20 s apart");
    // (and the same fake 21 s on is taken: a line)
    lobby_of_two(&mut k, 0xB6);
    k.from(V(1), &raceres(101, 0xB6, &fake));
    later(&mut k);
    let l = cheats(&k);
    assert!(l.len() == 1 && l[0].starts_with("[cheat] 10.77.0.10 \"Ann\": won the race of lobby 000000b6"), "{l:?}");
}

// a URL race of 3 heats: each racer's game sends a report a heat, the last heat's first, all at once, and each heat is
// judged as a race of its own; in heat 2 Ann's game had Bob's car jump 280 m once: a line naming him and the heat (the
// only other game of the race); a heat's second report of the same player counts for nothing
#[test]
fn every_heat_is_judged_and_a_jump_is_a_line() {
    let mut k = two();
    lobby_of_two(&mut k, 0xC1);
    let own = |me: u32, id: u32| 1 | if me == id { 2 } else { 0 };
    let rows = |me: u32, a: f32, b: f32| [(101, own(me, 101), a, 1.0, 5000.0), (100, own(me, 100), b, 1.0, 5000.0)];
    for (heat, a, b, jump) in [(3u8, 157.3, 167.9, 0u16), (1, 93.9, 94.8, 0), (2, 187.9, 206.4, 280)] {
        k.from(V(0), &raceres_heat(100, 0xC1, heat, 3, &rows(100, a, b), &[jump, 0]));
        k.from(V(1), &raceres_heat(101, 0xC1, heat, 3, &rows(101, a, b), &[0, 0]));
    }
    // (Bob again, heat 2, his car with a jump of its own: refused, the heat's report came)
    k.from(V(1), &raceres_heat(101, 0xC1, 2, 3, &rows(101, 187.9, 206.4), &[0, 900]));
    k.lines.lock().clear();
    k.hub.tick();
    assert_eq!(
        cheats(&k),
        ["[cheat] 10.77.0.11 \"Bob\": its car jumped 280 m at once in the race of lobby 000000c1 (race 7c80e775: sprint, 1 lap, \
          heat 2 of 3), as 1 of 1 other players' games had it (the largest each: 280 m; honest races' largest are under 50 \
          m); it won"]
    );
    let mut races: Vec<String> = k.lines.lock().iter().filter(|l| l.starts_with("[race] lobby 000000c1")).cloned().collect();
    races.sort();
    assert_eq!(races.len(), 3, "a result line a heat: {races:?}");
    assert!(races[0].contains("(sprint, 1 lap, heat 1 of 3): the result by 2 of 2 racers' games: 1 Bob 1:33.9"), "{races:?}");
}

// Bob's game says his car was in the air for 2 minutes (runtime/roam_game.c fly_frame), and his page leaves the switch:
// one line, with where his car was last and how many others were within 200 m along the ground (Ann, under him; not
// Cat, 300 m off), once a page; a page whose player is not on the hub has its name on the switch
#[test]
fn a_car_in_the_air_for_minutes_is_one_line_a_page() {
    let mut k = Kit::new(RoamOptions::default());
    hello(&mut k, 0, b"Ann", 2);
    hello(&mut k, 1, b"Bob", 2);
    hello(&mut k, 2, b"Cat", 2);
    k.hub.page_name(V(1), "Bob".into());
    k.from(V(0), &snap(100, [150.0, 0.0, 0.0]));
    k.from(V(1), &snap(101, [0.0, 0.0, 40.0]));
    k.from(V(2), &snap(102, [300.0, 0.0, 0.0]));
    k.hub.flew(V(1), 120.0);
    k.hub.flew(V(1), 120.0);
    assert_eq!(
        cheats(&k),
        ["[cheat] 10.77.0.11 \"Bob\": its car was in the air in free roam for 120 s, by its own game, last at 0 0 40, 1 other \
          player within 200 m: its page left the switch"]
    );
    k.hub.page_name(V(5), "Eve".into());
    k.hub.flew(V(5), f64::NAN);
    assert_eq!(
        cheats(&k)[1],
        "[cheat] 10.77.0.15 \"Eve\": its car was in the air in free roam for 0 s, by its own game, not on the hub now: its \
         page left the switch"
    );
}

/// a racer: its browser, its endpoint (port), its game's id and name, and its client (an opaque id of its address, as
/// the switch gives the hub with each frame; None: its browser is a client of its own)
#[derive(Clone, Copy)]
struct Seat {
    vip: u32,
    port: u16,
    id: u32,
    name: &'static [u8],
    client: Option<&'static str>,
}

fn seat(vip: u32, id: u32, name: &'static [u8]) -> Seat {
    Seat { vip, port: 47210, id, name, client: None }
}

/// a datagram from a racer's endpoint
fn at(k: &mut Kit, s: Seat, d: &[u8]) {
    let client: Option<Arc<str>> = s.client.map(Arc::from);
    k.hub.frame_of(s.vip, &frame_ports(s.vip, d, s.port, 47210), None, client.as_ref());
}

/// a racer's HELLO
fn hello_of(k: &mut Kit, s: Seat) {
    at(k, s, &hello_with(s.id, &HelloOpts { core: 5, room: fnv1a("bay"), name: s.name, ..Default::default() }));
}

/// a race of lobby `l` (race 0x7C80E775, a sprint of 1 lap) that `a` hosts and `b` joins, after their HELLOs; both games
/// report it alike, a row (id, finished, time, laps, m) a racer; then the hub's tick, and 21 s on (a player's results
/// are taken 20 s after its last)
fn race(k: &mut Kit, l: u32, a: Seat, b: Seat, rows: &[(u32, bool, f32, f32, f32)]) {
    hello_of(k, a);
    hello_of(k, b);
    at(k, a, &lobby(a.id, 1, l));
    at(k, b, &lobby(b.id, 2, l));
    for s in [a, b] {
        let mine: Vec<_> = rows.iter().map(|&(id, fin, t, done, m)| (id, fin as u8 | if id == s.id { 2 } else { 0 }, t, done, m)).collect();
        at(k, s, &raceres(s.id, l, &mine));
    }
    k.hub.tick();
    k.step(21_000.0);
}

/// the `[race]` result lines of lobby `l`
fn results_of(k: &Kit, l: u32) -> Vec<String> {
    k.lines.lock().iter().filter(|x| x.starts_with(&format!("[race] lobby {l:08x}, "))).cloned().collect()
}

// The 2026-11-05 security audit: a game says its own id, and any PEERS list has the others'.  Mal's browser (V(1)) has
// two endpoints: its first hosts a lobby, its second says Vic's id and joins it; the first's report has Vic's car jump
// 300 m.  Vic (V(5)) never raced: the lines name the endpoint that joined with that id, Mal's second.
#[test]
fn a_borrowed_id_names_the_borrower() {
    let mut k = Kit::new(RoamOptions::default());
    let vic = seat(V(5), 0x5555, b"Vic");
    let (mal, mal2) = (seat(V(1), 0x1111, b"Mal"), Seat { port: 47211, ..seat(V(1), 0x5555, b"Mal 2") });
    hello_of(&mut k, vic);
    k.from(V(5), &snap(0x5555, [0.0, 0.0, 0.0]));
    hello_of(&mut k, mal);
    hello_of(&mut k, mal2);
    at(&mut k, mal, &lobby(0x1111, 1, 0xA1));
    at(&mut k, mal2, &lobby(0x5555, 2, 0xA1));
    let own = |me: u32, id: u32| if me == id { 2 } else { 0 };
    let rows = |me: u32| [(0x1111, 1 | own(me, 0x1111), 60.0, 1.0, 3000.0), (0x5555, 1 | own(me, 0x5555), 61.0, 1.0, 3000.0)];
    at(&mut k, mal, &raceres_heat(0x1111, 0xA1, 0, 0, &rows(0x1111), &[0, 300]));
    at(&mut k, mal2, &raceres_heat(0x5555, 0xA1, 0, 0, &rows(0x5555), &[0, 0]));
    k.hub.tick();
    let l = cheats(&k);
    assert!(
        l.len() == 1 && l[0].starts_with("[cheat] 10.77.0.11 \"Mal 2\": its car jumped 300 m at once in the race of lobby 000000a1"),
        "{l:?}"
    );
    let r = results_of(&k, 0xA1);
    assert!(r.len() == 1 && r[0].ends_with("the result by 2 of 2 racers' games: 1 Mal 1:00.0, 2 Mal 2 1:01.0"), "{r:?}");
    assert!(k.lines.lock().iter().all(|x| !x.contains("Vic") && !x.contains("10.77.0.15")), "{:?}", k.lines.lock());
}

// ...and an id is the first browser's to join a lobby with: Vic joins Mal's lobby, then Mal's second endpoint asks to
// join it under Vic's id and reports first as Vic's game, that Vic won at 0.6 % of the way (a race the game ended at
// once), as Mal's own report does.  Its ask and its report count for nothing; Vic's own report (Mal won, Vic 5 s
// behind) is taken, and the two reports name no winner.
#[test]
fn an_id_is_the_first_browsers_to_join_with_it() {
    let mut k = Kit::new(RoamOptions::default());
    let vic = seat(V(5), 0x5555, b"Vic");
    let (mal, mal2) = (seat(V(1), 0x1111, b"Mal"), Seat { port: 47211, ..seat(V(1), 0x5555, b"Mal 2") });
    hello_of(&mut k, vic);
    hello_of(&mut k, mal);
    at(&mut k, mal, &lobby(0x1111, 1, 0xA2));
    at(&mut k, vic, &lobby(0x5555, 2, 0xA2));
    hello_of(&mut k, mal2);
    at(&mut k, mal2, &lobby(0x5555, 2, 0xA2));
    let own = |me: u32, id: u32| if me == id { 2 } else { 0 };
    let framed = |me: u32| [(0x5555, own(me, 0x5555), 4.75, 0.006, 38.0), (0x1111, own(me, 0x1111), 34.75, 0.168, 901.0)];
    at(&mut k, mal, &raceres(0x1111, 0xA2, &framed(0x1111)));
    at(&mut k, mal2, &raceres(0x5555, 0xA2, &framed(0x5555)));
    at(&mut k, vic, &raceres(0x5555, 0xA2, &[(0x1111, 1, 180.0, 1.0, 5800.0), (0x5555, 3, 185.0, 1.0, 5810.0)]));
    k.hub.tick();
    assert_eq!(cheats(&k), Vec::<String>::new());
    let r = results_of(&k, 0xA2);
    assert!(r.len() == 1 && r[0].ends_with("the result by 2 of 2 racers' games: 1 Mal 3:00.0, 2 Vic 3:05.0"), "{r:?}");
}

// The 2026-11-05 security audit: a race's clean wins are what its winners are measured by, and two endpoints of one
// browser "raced" a race no one had won yet, both reports alike, to a slow clean win (33:20); then an honest winner far
// ahead of the next car was a line.  A clean win is by the reports of two clients: one browser's endpoints are one, and
// so are two browsers of one address.  Two players of two addresses race honestly: each race is a `[race]` line, and
// their clean wins are the race's (10 of two winners: a teleport to the finish is a line).
#[test]
fn a_clean_win_is_by_two_clients() {
    let mut k = Kit::new(RoamOptions::default());
    let slow = |w: Seat, o: Seat, n: u32| [(w.id, true, 2000.0 + n as f32, 1.0, 9000.0), (o.id, true, 2100.0 + n as f32, 1.0, 9000.0)];
    // (one browser's two endpoints, and two browsers of one address: 10 races each, the two winning in turn)
    let m = (seat(V(1), 0x1111, b"m1"), Seat { port: 47211, ..seat(V(1), 0x2222, b"m2") });
    let n = (Seat { client: Some("one"), ..seat(V(3), 0x3333, b"n1") }, Seat { client: Some("one"), ..seat(V(4), 0x4444, b"n2") });
    for i in 0..10 {
        for (l, (a, b)) in [(0xA0 + i, m), (0xB0 + i, n)] {
            let (w, o) = if i % 2 == 0 { (a, b) } else { (b, a) };
            race(&mut k, l, a, b, &slow(w, o, i));
        }
    }
    // Ann (V(5)) wins in 1:00, Bob (V(6)) gave up at 60 % of the way
    let (ann, bob) = (seat(V(5), 100, b"Ann"), seat(V(6), 101, b"Bob"));
    race(&mut k, 0xC0, ann, bob, &[(100, true, 60.0, 1.0, 3000.0), (101, false, 50.0, 0.6, 1800.0)]);
    assert_eq!(cheats(&k), Vec::<String>::new());
    let r = results_of(&k, 0xC0);
    assert!(r[0].ends_with("the result by 2 of 2 racers' games: 1 Ann 1:00.0, DNF Bob"), "{r:?}");
    // 10 honest races, Ann and Bob winning in turn; then Bob in 0:21, Ann still on the way
    for i in 0..10 {
        let (w, o) = if i % 2 == 0 { (ann, bob) } else { (bob, ann) };
        race(&mut k, 0xD0 + i, ann, bob, &[(w.id, true, 180.0 + i as f32, 1.0, 5800.0), (o.id, true, 185.0 + i as f32, 1.0, 5810.0)]);
    }
    let r = results_of(&k, 0xD1);
    assert!(r[0].ends_with("the result by 2 of 2 racers' games: 1 Bob 3:01.0, 2 Ann 3:06.0"), "{r:?}");
    assert_eq!(cheats(&k), Vec::<String>::new());
    race(&mut k, 0xDA, ann, bob, &[(101, true, 21.0, 1.0, 400.0), (100, false, 51.0, 0.56, 1624.0)]);
    assert_eq!(
        cheats(&k),
        ["[cheat] 10.77.0.16 \"Bob\": won the race of lobby 000000da (race 7c80e775: sprint, 1 lap) in 0:21.0, under 50 % of \
          the race's quickest clean win (3:00.0); nobody else finished (the race ends 30 s after the first): as 2 of 2 \
          players' games saw it"]
    );
}

// A race's quickest clean win goes for a teleport to the finish (nobody else over the line) once the race had 10
// clean wins by two winners at least, as the third quickest winner's does (the 2026-11-05 security audit: from the
// first, a slow "race" of a race no one had won yet, 33:20, made every honest winner in under half of it with no car
// close behind a line, and a win with a line is never a clean win that brings it down).  Cid and Dee (two addresses)
// agree on such a race: Ann's win in 1:00 is no line.
#[test]
fn the_quickest_clean_win_waits_for_ten_by_two_winners() {
    let mut k = Kit::new(RoamOptions::default());
    let (cid, dee) = (seat(V(7), 0x7777, b"Cid"), seat(V(8), 0x8888, b"Dee"));
    let (ann, bob) = (seat(V(5), 100, b"Ann"), seat(V(6), 101, b"Bob"));
    race(&mut k, 0xB0, cid, dee, &[(0x7777, true, 2000.0, 1.0, 9000.0), (0x8888, true, 2100.0, 1.0, 9000.0)]);
    race(&mut k, 0xC0, ann, bob, &[(100, true, 60.0, 1.0, 3000.0), (101, false, 50.0, 0.6, 1800.0)]);
    assert_eq!(cheats(&k), Vec::<String>::new(), "one clean win");
    // (9 more, all Cid's, 3:00 the quickest)
    for i in 1..10 {
        race(&mut k, 0xB0 + i, cid, dee, &[(0x7777, true, 179.0 + i as f32, 1.0, 5800.0), (0x8888, true, 185.0 + i as f32, 1.0, 5810.0)]);
    }
    let tele = [(101, true, 21.0, 1.0, 400.0), (100, false, 51.0, 0.56, 1624.0)];
    race(&mut k, 0xC1, ann, bob, &tele);
    assert_eq!(cheats(&k), Vec::<String>::new(), "10 clean wins, all Cid's");
    race(&mut k, 0xBA, cid, dee, &[(0x8888, true, 190.0, 1.0, 5800.0), (0x7777, true, 195.0, 1.0, 5810.0)]);
    race(&mut k, 0xC2, ann, bob, &tele);
    let l = cheats(&k);
    assert!(
        l.len() == 1
            && l[0].starts_with(
                "[cheat] 10.77.0.16 \"Bob\": won the race of lobby 000000c2 (race 7c80e775: sprint, 1 lap) in 0:21.0, under 50 % \
                 of the race's quickest clean win (3:00.0); "
            ),
        "{l:?}"
    );
}
