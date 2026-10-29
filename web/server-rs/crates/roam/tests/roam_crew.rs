//! Crews (roam_core.h RC_HF_CREW): a player's HELLO names its crews (a word each, up to 4, by roam_core's room hash),
//! the hub links two crew-mates of one room before the other links, and /players counts each crew's players.
//! 1. the wire: a HELLO of before parses as it did; one with the flag names 0 to 4 crews after its ids, and what
//!    comes after them is skipped; a count past 4, a crew 0 or a short one is a bad HELLO;
//! 2. the links (every pair | the grid | a room past exactRoom): a crew-mate 2 km off a crowd is linked, under the
//!    busy cap too, and one of another crew or another room is not; at most 16 crew links a player, the nearest, kept
//!    while the crew drives about; a crew of 65 in one room is no reason for a link;
//! 3. the counts: the browsers in free roam or paused there, by crew, in any room, a browser once; on the hub's thread
//!    as of its last tick;
//! 4. the lists' crew marks (roam_core.h RC_HF_HUBCREW): a player that reads them and is in a crew learns which of its
//!    peers share one of its crews, and no other player's crews; the others get the lists of before.

mod kit;

use std::collections::HashSet;
use std::sync::Arc;
use std::time::{Duration, Instant};

use bytes::Bytes;
use kit::*;
use nfsu2_roam::{RoamHub, RoamMsg, RoamOptions, decode_roam, encode_peers};
use nfsu2_util::{FrameCtx, Host};

/// a HELLO as roam_core.c lays it out (core version 7, in free roam, room "blue", named "p") with these header flags:
/// its count of ids, the ids, then `tail` as it is
fn hello_raw(flags: u16, n_seen: u8, seen: &[u32], tail: &[u8]) -> Vec<u8> {
    let mut p = vec![0u8; 12];
    p[0..2].copy_from_slice(&7u16.to_le_bytes());
    p[2] = 1;
    p[3] = 2;
    p[4..6].copy_from_slice(&47210u16.to_le_bytes());
    p[6..8].copy_from_slice(&3u16.to_le_bytes());
    p[8..12].copy_from_slice(&fnv1a("blue").to_le_bytes());
    p.extend_from_slice(&[1, b'p', n_seen]);
    for s in seen {
        p.extend_from_slice(&s.to_le_bytes());
    }
    p.extend_from_slice(tail);
    dgram_flags(1, 0x77, flags, &p)
}

/// what a HELLO ends with after its ids: a count, then hashes
fn tail(n: u8, crews: &[u32]) -> Vec<u8> {
    let mut t = vec![n];
    for c in crews {
        t.extend_from_slice(&c.to_le_bytes());
    }
    t
}

/// the crews a datagram's HELLO names (None: no datagram)
fn crews_in(d: &[u8]) -> Option<Vec<u32>> {
    match decode_roam(d)? {
        RoamMsg::Hello { crews, .. } => Some(crews),
        m => panic!("not a HELLO: {m:?}"),
    }
}

// the wire: a HELLO of before (no flag) parses as before, whatever follows its ids; with the flag, 0 to 4 crews after
// its ids (or nothing), what comes after them skipped; a count past 4, a crew 0, crews or ids cut short, or more ids
// than roam_core takes, is a bad HELLO
#[test]
fn the_wire_a_hello_names_its_crews_after_its_ids() {
    let (a, b, c, d) = (fnv1a("bluebirds"), fnv1a("owls"), 7, 0xFFFF_FFFF);
    let ids = [0x11, 0x22];
    assert_eq!(crews_in(&hello_raw(0, 2, &ids, &[])), Some(vec![]), "a HELLO of before");
    assert_eq!(crews_in(&hello_raw(0, 2, &ids, &tail(9, &[0]))), Some(vec![]), "without the flag: what follows its ids is not read");
    assert_eq!(crews_in(&hello_raw(0, 9, &ids, &[])), Some(vec![]), "without the flag: ids cut short, as before");
    assert_eq!(crews_in(&hello_raw(0, 40, &[1; 40], &[])), Some(vec![]), "without the flag: 40 ids, as before");
    assert_eq!(crews_in(&hello_with(0x77, &HelloOpts { seen: &ids, ..Default::default() })), Some(vec![]), "the kit's HELLO of before");
    assert_eq!(crews_in(&hello_raw(HF_CREW, 2, &ids, &[])), Some(vec![]), "the flag, nothing after its ids");
    assert_eq!(crews_in(&hello_raw(HF_CREW, 0, &[], &[])), Some(vec![]), "the flag, no ids, nothing after them");
    // quick chat's flag (roam_core.h RC_HF_SAY, the bit below the crews'): bytes after its ids are not crews
    assert_eq!(crews_in(&hello_raw(0x0100, 2, &ids, &tail(1, &[0x1234]))), Some(vec![]), "RC_HF_SAY is not the crew flag");
    for crews in [&[][..], &[a], &[a, b], &[d, c, b], &[a, b, c, d], &[b, b]] {
        let want = Some(crews.to_vec());
        assert_eq!(crews_in(&hello_raw(HF_CREW, 2, &ids, &tail(crews.len() as u8, crews))), want, "{crews:?}");
        assert_eq!(crews_in(&hello_raw(HF_CREW | 0x0080, 0, &[], &tail(crews.len() as u8, crews))), want, "no ids: {crews:?}");
        let mut more = tail(crews.len() as u8, crews);
        more.extend_from_slice(&[1, 2, 3, 4, 5, 6, 7]);
        assert_eq!(crews_in(&hello_raw(HF_CREW, 2, &ids, &more)), want, "what follows its crews skipped: {crews:?}");
        let opts = HelloOpts { seen: &ids, crews: Some(crews), ..Default::default() };
        assert_eq!(crews_in(&hello_with(0x77, &opts)), want, "the kit's: {crews:?}");
    }
    for (what, h) in [
        ("5 crews", hello_raw(HF_CREW, 2, &ids, &tail(5, &[a, b, c, d, 9]))),
        ("a count of 255", hello_raw(HF_CREW, 2, &ids, &tail(255, &[a]))),
        ("a crew 0", hello_raw(HF_CREW, 2, &ids, &tail(2, &[a, 0]))),
        ("crews cut short", hello_raw(HF_CREW, 2, &ids, &tail(2, &[a]))),
        ("a crew cut short", hello_raw(HF_CREW, 2, &ids, &tail(1, &[a])[..3])),
        ("ids cut short", hello_raw(HF_CREW, 3, &ids, &[])),
        ("33 ids", hello_raw(HF_CREW, 33, &[1; 33], &tail(1, &[a]))),
    ] {
        assert_eq!(crews_in(&h), None, "{what}: a bad HELLO");
    }
    // the hub counts it bad, and takes no player of it
    let mut k = Kit::new(RoamOptions::default());
    k.from(V(0), &hello_raw(HF_CREW, 2, &ids, &tail(2, &[a, 0])));
    assert_eq!((k.info("bad"), k.info("players")), (1.0, 0.0));
    k.from(V(0), &hello_raw(HF_CREW, 2, &ids, &tail(2, &[a, b])));
    assert_eq!((k.info("bad"), k.info("players")), (1.0, 1.0));
}

// the lists' crew marks: to a player whose HELLO says it reads them (core 8) and that is in a crew, its own crews, then
// a mark a link, bit c: that player is in its crew c too (whatever its core: the hub has its crews); a player of core 7
// or in no crew gets a list of before, byte for byte; and no list carries a crew of anyone but its receiver
#[test]
fn the_lists_mark_crew_mates_and_name_no_other_crews() {
    let (x, y, z, w) = (fnv1a("bluebirds"), fnv1a("owls"), fnv1a("hawks"), fnv1a("wrens"));
    for (how, o) in ways() {
        let mut k = Kit::new(o);
        // A (x, y), B (y, z), C a core 7 (z, x), D in no crew, E (z, w): all within 40 m, so all linked
        let who: [(Option<&[u32]>, bool); 5] =
            [(Some(&[x, y]), true), (Some(&[y, z]), true), (Some(&[z, x]), false), (None, true), (Some(&[z, w]), true)];
        for (i, &(crews, hubcrew)) in who.iter().enumerate() {
            let i = i as u32;
            k.from(V(i), &hello_with(100 + i, &HelloOpts { core: if hubcrew { 8 } else { 7 }, crews, hubcrew, ..Default::default() }));
            k.from(V(i), &snap(100 + i, [i as f64 * 10.0 + 0.25, 0.25, 0.0]));
        }
        k.hub.tick();
        let sent = k.sent.lock().clone();
        let of =
            |i: u32| sent.iter().find(|(vip, _)| *vip == V(i)).map(|(_, f)| f.clone()).unwrap_or_else(|| panic!("{how}: a list to {i}"));
        let mark = |i: u32, j: u32| {
            let f = of(i);
            let at = peers_of(&f).iter().position(|e| e.vip == V(j)).unwrap_or_else(|| panic!("{how}: {j} on {i}'s list"));
            crew_marks(&f).unwrap_or_else(|| panic!("{how}: marks on {i}'s list")).1[at]
        };
        for i in 0..5 {
            assert_eq!(peers_of(&of(i)).len(), 4, "{how}: {i} linked with the 4 others");
        }
        assert_eq!(crew_marks(&of(0)).map(|m| m.0), Some(vec![x, y]), "{how}: A's own crews");
        assert_eq!(
            [mark(0, 1), mark(0, 2), mark(0, 3), mark(0, 4)],
            [0b10, 0b01, 0, 0],
            "{how}: B in y, C in x (a core 7: the hub has its crews)"
        );
        assert_eq!(crew_marks(&of(1)).map(|m| m.0), Some(vec![y, z]), "{how}: B's own crews");
        assert_eq!([mark(1, 0), mark(1, 2), mark(1, 3), mark(1, 4)], [0b01, 0b10, 0, 0b10], "{how}: A in y, C and E in z");
        assert_eq!([mark(4, 0), mark(4, 1), mark(4, 2), mark(4, 3)], [0, 0b01, 0b01, 0], "{how}: E (z, w): B and C in z");
        for (i, why) in [(2, "a core 7"), (3, "in no crew")] {
            let f = of(i);
            assert_eq!(crew_marks(&f), None, "{how}: {why}: no marks");
            assert_eq!(
                &f[16..],
                &encode_peers(0xB0B, u16::from_le_bytes([f[28], f[29]]), &peers_of(&f))[..],
                "{how}: {why}: a list of before"
            );
        }
        // no crew of anyone but the receiver in its datagram (C's x and z are A's and B's too: w is E's alone)
        let has = |f: &[u8], h: u32| f.windows(4).any(|b| b == h.to_le_bytes());
        for i in 0..4 {
            assert!(!has(&of(i)[16..], w), "{how}: E's crew w on {i}'s list");
        }
        assert!(!has(&of(0)[16..], z) && !has(&of(3)[16..], x) && !has(&of(2)[16..], y), "{how}: no crews but its own");
    }
}

/// a player of core version 7 at (x, y), with these crews (None: a HELLO of before), its HELLO and its SNAP
fn put(k: &mut Kit, vip: u32, id: u32, room: u32, crews: Option<&[u32]>, at: [f64; 2]) {
    k.from(vip, &hello_with(id, &HelloOpts { core: 6, room, crews, ..Default::default() }));
    k.from(vip, &snap(id, [at[0], at[1], 0.0]));
}

/// player i's address, for more players than V's: 10.77.1.0 and on
#[allow(non_snake_case)]
fn U(i: u32) -> u32 {
    0x0A4D_0100 + i
}

/// whether players a and b (addresses) are linked, checked at both ends
fn linked(k: &Kit, a: u32, b: u32) -> bool {
    let l = k.lists();
    let has = |x: u32, y: u32| l.get(&format!("{x}:47210")).is_some_and(|v| v.iter().any(|e| e.vip == y));
    assert_eq!(has(a, b), has(b, a), "a link is listed at both ends");
    has(a, b)
}

/// the ways a room is linked: every pair (a room of at most exactRoom), the grid (exactRoom 1), and the grid for a
/// room larger than exactRoom (300 players), with the crowd's size each; without view points (the cars point along x)
fn crowds() -> [(&'static str, RoamOptions, u32); 3] {
    let o = RoamOptions { view_m: 1e-9, ..RoamOptions::default() };
    [("every pair", o.clone(), 60), ("the grid", RoamOptions { exact_room: 1, ..o.clone() }, 60), ("a room past exactRoom", o, 300)]
}

/// A crowd: player 0 (A, crews `a`) and `n` players around it within 50 m (lattice points 3 m apart, HELLOs without
/// crews), and B, 2 km off, crews `b`, in `b_room`: their HELLOs and SNAPs
fn crowd(k: &mut Kit, n: u32, a: &[u32], b: &[u32], b_room: u32) {
    put(k, U(0), 1000, 0, Some(a), [0.25, 0.25]);
    let side = (n as f64).sqrt().ceil() as u32;
    for i in 1..=n {
        let (x, y) = (((i - 1) % side) as f64 * 3.0, ((i - 1) / side) as f64 * 3.0);
        let half = (side - 1) as f64 * 1.5;
        put(k, U(i), 1000 + i, 0, None, [x - half + 1.25, y - half + 1.25]);
    }
    put(k, U(n + 1), 1000 + n + 1, b_room, Some(b), [2000.25, 0.25]);
}

// a crew-mate 2 km off a crowd (60 | 300 players packed around A, who has 32 nearer players) is linked with A, in one
// of its crews or in all; one in no crew of A's is not, nor a crew-mate in another room
#[test]
fn a_crew_mate_far_off_a_crowd_is_linked_first() {
    let (x, y, z) = (fnv1a("bluebirds"), fnv1a("owls"), fnv1a("hawks"));
    for (how, o, n) in crowds() {
        let b = U(n + 1);
        for (crews_a, crews_b, b_room, want) in [
            (&[x][..], &[y, x][..], 0, true),
            (&[x, z], &[z, y], 0, true),
            (&[x], &[x], 0, true),
            (&[x], &[y], 0, false),
            (&[], &[x], 0, false),
            (&[x], &[x], fnv1a("red"), false),
        ] {
            let what = format!("{how}: A {crews_a:?}, B {crews_b:?} in room {b_room}");
            let mut k = Kit::new(o.clone());
            crowd(&mut k, n, crews_a, crews_b, b_room);
            k.hub.tick();
            assert_eq!(linked(&k, U(0), b), want, "{what}");
            assert_eq!(k.info("crewLinks"), if want { 1.0 } else { 0.0 }, "{what}");
            assert_eq!(k.lists()[&format!("{}:47210", U(0))].len(), 32, "{what}: A's links are full");
        }
        // B leaves the crew (a HELLO without it), and at the next tick (the link's hold over) it is linked no more
        let mut k = Kit::new(o.clone());
        crowd(&mut k, n, &[x], &[x], 0);
        k.hub.tick();
        assert!(linked(&k, U(0), b), "{how}");
        for _ in 0..6 {
            k.step(1000.0);
            crowd(&mut k, n, &[x], &[y], 0);
            k.clear();
            k.hub.tick();
        }
        assert!(!linked(&k, U(0), b), "{how}: B left the crew");
        assert_eq!(k.info("crewLinks"), 0.0, "{how}");
    }
}

// under the busy cap (8 links a player) a crew-mate 2 km off the crowd is still linked
#[test]
fn a_crew_mate_far_off_is_linked_under_the_busy_cap() {
    let x = fnv1a("bluebirds");
    for (how, o, n) in crowds() {
        let mut k = Kit::new(RoamOptions { cap_down_ms: 1.0, ..o });
        *k.busy.lock() = 0.95;
        // the cap a quarter down each tick: 24, 18, 13, 9, 8
        for _ in 0..6 {
            crowd(&mut k, n, &[x], &[x], 0);
            k.clear();
            k.hub.tick();
            k.step(1000.0);
        }
        assert_eq!(k.info("cap"), 8.0, "{how}");
        assert!(linked(&k, U(0), U(n + 1)), "{how}: linked under the busy cap");
        assert_eq!(k.info("crewLinks"), 1.0, "{how}");
        let l = k.lists();
        assert!(l.values().all(|v| v.len() <= 8), "{how}: 8 links at most");
        assert_eq!(l[&format!("{}:47210", U(0))].len(), 8, "{how}: A's 8 links: B and 7 of the crowd");
    }
}

/// A crew of 20 spread out (members 300 m apart on a line, member m at the `place(m)`-th spot of it), each member
/// with 31 players packed around it (within 3 m: they fill its links once its crew's are in, so its links to
/// crew-mates are its crew links): their HELLOs and SNAPs
fn crew_of_20(k: &mut Kit, crew: u32, place: impl Fn(u32) -> u32) {
    for m in 0..20u32 {
        let x = place(m) as f64 * 300.0 + 0.25;
        put(k, U(m * 32), 5000 + m * 32, 0, Some(&[crew]), [x, 0.25]);
        for s in 1..32u32 {
            let (dx, dy) = ((s % 6) as f64 * 0.5, (s / 6) as f64 * 0.5 + 0.25);
            put(k, U(m * 32 + s), 5000 + m * 32 + s, 0, None, [x + dx, dy]);
        }
    }
}

/// the crew-mates each member of `crew_of_20` is linked with, by member
fn mates_of_20(k: &Kit) -> Vec<HashSet<u32>> {
    let l = k.lists();
    let members: HashSet<u32> = (0..20u32).map(|m| U(m * 32)).collect();
    (0..20u32)
        .map(|m| {
            let mine = &l[&format!("{}:47210", U(m * 32))];
            mine.iter().filter(|e| members.contains(&e.vip)).map(|e| (e.vip - U(0)) / 32).collect()
        })
        .collect()
}

// a crew of 20 spread out: each member gets at most 16 crew links, nearest first (a pair is left out only when one of
// its two has 16 nearer ones), and they stay while the crew drives about
#[test]
fn a_crew_of_20_at_most_16_crew_links_each_the_nearest() {
    let crew = fnv1a("bluebirds");
    let o = RoamOptions { view_m: 1e-9, ..RoamOptions::default() };
    for (how, o) in [("every pair", RoamOptions { exact_room: 1000, ..o.clone() }), ("the grid", RoamOptions { exact_room: 1, ..o })] {
        let mut k = Kit::new(o);
        crew_of_20(&mut k, crew, |m| m);
        k.hub.tick();
        let links = mates_of_20(&k);
        let total: usize = links.iter().map(HashSet::len).sum();
        assert_eq!(k.info("crewLinks"), (total / 2) as f64, "{how}: every crew-mate link is a crew link");
        for (m, l) in links.iter().enumerate() {
            assert!(l.len() <= 16, "{how}: member {m}: {} crew links", l.len());
            for &j in l {
                assert!(links[j as usize].contains(&(m as u32)), "{how}: both ends");
            }
        }
        // the middle ones: their 8 nearest on each side
        for m in [9u32, 10] {
            let want: HashSet<u32> = (m - 8..=m + 8).filter(|&j| j != m && j < 20).collect();
            assert_eq!(links[m as usize], want, "{how}: member {m}");
        }
        // nearest first: a pair left out has an end with 16 nearer (or as near) crew links
        let gap = |a: u32, b: u32| a.abs_diff(b);
        for a in 0..20u32 {
            for b in a + 1..20 {
                if !links[a as usize].contains(&b) {
                    let full = |m: u32| links[m as usize].len() == 16 && links[m as usize].iter().all(|&j| gap(m, j) <= gap(a, b));
                    assert!(full(a) || full(b), "{how}: {a} and {b} not linked: {:?} {:?}", links[a as usize], links[b as usize]);
                }
            }
        }
        // a second later the members are in other places (each one's nearest are others): the same crew links, as
        // links younger than holdMs stay
        k.step(1000.0);
        k.clear();
        crew_of_20(&mut k, crew, |m| m * 7 % 20);
        k.hub.tick();
        assert_eq!(mates_of_20(&k), links, "{how}: kept while the crew drives about");
    }
}

// a crew of 65 players in one room gets no links first (counted in crewSkips), and its players link by distance, the
// first and the last of them (6.4 km apart) as a crew of their own too; one of 64 gets them (and its first and last
// had their 16 crew links nearer)
#[test]
fn a_crew_of_65_in_one_room_gets_no_links_first() {
    let crew = fnv1a("everyone");
    for (how, o) in ways() {
        for (n, skips) in [(64u32, 0.0), (65, 1.0)] {
            let mut k = Kit::new(o.clone());
            for i in 0..n {
                put(&mut k, U(i), 100 + i, 0, Some(&[crew]), [i as f64 * 100.0 + 0.25, 0.25]);
            }
            put(&mut k, U(0), 100, 0, Some(&[crew, 77]), [0.25, 0.25]);
            put(&mut k, U(n - 1), 100 + n - 1, 0, Some(&[77, crew]), [(n - 1) as f64 * 100.0 + 0.25, 0.25]);
            k.hub.tick();
            assert_eq!(k.info("crewSkips"), skips, "{how}: a crew of {n}");
            if n == 64 {
                assert!(k.info("crewLinks") > 400.0, "{how}: a crew of {n}: {} crew links", k.info("crewLinks"));
            } else {
                assert_eq!(k.info("crewLinks"), 1.0, "{how}: a crew of {n}: only the two's");
                assert!(linked(&k, U(0), U(n - 1)), "{how}: a crew of {n}: the two, 6.4 km apart");
            }
        }
    }
}

// players in crews come and go every way (a HELLO without them, BYE, gone from the switch, silent past goneMs, another
// start of the game on the same endpoint): with none left the tick looks for no crews, and the next ones are linked
// first again (every pair | the grid)
#[test]
fn players_in_crews_come_and_go() {
    let x = fnv1a("bluebirds");
    for (how, o, n) in crowds().into_iter().take(2) {
        let mut k = Kit::new(o);
        let (a, b) = (U(0), U(n + 1));
        crowd(&mut k, n, &[x], &[x], 0);
        k.hub.tick();
        assert!(linked(&k, a, b), "{how}");
        // B: a HELLO of before, then back in the crew, then BYE; A: gone from the switch
        k.from(b, &hello_with(1000 + n + 1, &HelloOpts { core: 6, ..Default::default() }));
        k.from(b, &hello_with(1000 + n + 1, &HelloOpts { core: 6, crews: Some(&[x]), ..Default::default() }));
        k.from(b, &bye(1000 + n + 1));
        k.hub.peer_gone(a);
        k.step(1000.0);
        k.clear();
        k.hub.tick();
        assert_eq!((k.info("crewLinks"), k.info("players")), (0.0, n as f64), "{how}");
        // both back, as another start of the game each (a new id on A's endpoint): linked first again
        k.step(1000.0);
        put(&mut k, a, 3000, 0, Some(&[x]), [0.25, 0.25]);
        put(&mut k, b, 3001, 0, Some(&[x]), [2000.25, 0.25]);
        put(&mut k, a, 3002, 0, Some(&[x]), [0.25, 0.25]);
        for i in 1..=n {
            put(&mut k, U(i), 1000 + i, 0, None, [i as f64 % 9.0 * 3.0 - 12.25, (i / 9) as f64 * 3.0 - 12.25]);
        }
        k.clear();
        k.hub.tick();
        assert!(linked(&k, a, b) && k.info("crewLinks") == 1.0, "{how}: back");
        // all of them silent past goneMs: forgotten, and the counts with them
        k.step(7000.0);
        k.hub.tick();
        assert_eq!((k.info("players"), k.hub.in_crews(&[x])[0]), (0.0, 0), "{how}");
        put(&mut k, a, 3003, 0, Some(&[x]), [0.25, 0.25]);
        put(&mut k, b, 3004, 0, Some(&[x]), [2000.25, 0.25]);
        k.clear();
        k.hub.tick();
        assert!(linked(&k, a, b) && k.info("crewLinks") == 1.0, "{how}: two of a crew alone");
    }
}

/// a HELLO of core version 7 in `room`, `status`, with these crews (None: a HELLO of before)
fn hello6(id: u32, room: u32, status: u8, crews: Option<&[u32]>) -> Vec<u8> {
    hello_with(id, &HelloOpts { core: 6, room, status, crews, ..Default::default() })
}

// /players' crews: the browsers in free roam or paused there (not in a race), by crew, in any room, a browser once
// whatever its endpoints and however often it names a crew; a new HELLO replaces a player's crews (one of before,
// none)
#[test]
fn in_free_roam_by_crew() {
    let (a, b, c) = (fnv1a("bluebirds"), fnv1a("owls"), fnv1a("hawks"));
    let mut k = Kit::new(RoamOptions::default());
    k.from(V(1), &hello6(601, fnv1a("blue"), 2, Some(&[a, b, a])));
    k.from(V(2), &hello6(602, 0, 5, Some(&[a])));
    k.from(V(3), &hello6(603, 0, 3, Some(&[a, b])));
    k.from(V(4), &hello6(604, fnv1a("red"), 2, Some(&[b])));
    k.from(V(5), &hello6(605, 0, 2, None));
    k.hub.frame(V(1), &frame_ports(V(1), &hello6(606, 0, 2, Some(&[a])), 47211, 47210));
    assert_eq!(k.hub.in_crews(&[a, b, c, a]), [2, 2, 0, 2]);
    assert_eq!(k.hub.in_crews(&[]), Vec::<usize>::new());
    assert_eq!(k.hub.in_roam(), 4);
    // 2 leaves its crews with a HELLO of before, 4 changes them, 3 is back from its race
    k.from(V(2), &hello6(602, 0, 5, None));
    k.from(V(4), &hello6(604, fnv1a("red"), 2, Some(&[c])));
    k.from(V(3), &hello6(603, 0, 2, Some(&[a, b])));
    assert_eq!(k.hub.in_crews(&[a, b, c]), [2, 2, 1]);
    k.hub.peer_gone(V(1));
    assert_eq!(k.hub.in_crews(&[a, b, c]), [1, 1, 1]);
}

fn ctx() -> FrameCtx {
    FrameCtx { group: Arc::from("client"), site: None, cause: None, slot: None, owner: None }
}

// on its thread the hub keeps the crews' counts of its last tick for /players
#[test]
fn on_its_thread_the_crews_counts_of_the_last_tick() {
    let (a, b) = (fnv1a("bluebirds"), fnv1a("owls"));
    let hub = RoamHub::spawn(RoamOptions { tick_ms: 20, ..RoamOptions::default() }, Arc::new(|_: &str| {}));
    assert_eq!(hub.in_crews(&[a, b]), [0, 0], "before its first tick");
    let post = |vip: u32, d: Vec<u8>| hub.frame(vip, Bytes::from(frame(vip, &d)), ctx());
    post(V(1), hello_with(701, &HelloOpts { core: 6, crews: Some(&[a, b]), ..Default::default() }));
    post(V(2), hello_with(702, &HelloOpts { core: 6, crews: Some(&[b]), ..Default::default() }));
    let t0 = Instant::now();
    while hub.in_crews(&[b, a, 9]) != [2, 1, 0] {
        assert!(t0.elapsed() < Duration::from_secs(5), "{:?}", hub.in_crews(&[b, a, 9]));
        std::thread::sleep(Duration::from_millis(5));
    }
    hub.peer_gone(V(1));
    let t0 = Instant::now();
    while hub.in_crews(&[b, a]) != [1, 0] {
        assert!(t0.elapsed() < Duration::from_secs(5), "{:?}", hub.in_crews(&[b, a]));
        std::thread::sleep(Duration::from_millis(5));
    }
    hub.stop();
}
