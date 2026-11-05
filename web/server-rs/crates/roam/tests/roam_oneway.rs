//! One-way links (roam_core version 15, hub.rs `one_way`): a player gets the cars of its nearest others, as many as it
//! shows, even where those show fewer (a computer's 32, a tablet's 24, a phone's 16); each sends its car to `sends`
//! players at most (64; its links under the busy cap); players of older cores, and the pairs linked first (outruns,
//! lobbies, crews, friends), keep links both ways.

mod kit;

use std::collections::HashMap;

use kit::*;
use nfsu2_roam::RoamOptions;

/// browser i's address, for more players than V's: 10.77.0.2 and on
#[allow(non_snake_case)]
fn W(i: u32) -> u32 {
    0x0A4D_0002 + i
}

/// the way marks of the hub's lists (roam_core.h RC_MARK_RXONLY / RC_MARK_TXONLY)
const RX: u8 = 0x20;
const TX: u8 = 0x40;

/// every player's last list: (vip, its mark) an entry; no marks: 0 each
fn lists_with_marks(k: &Kit) -> HashMap<u32, Vec<(u32, u8)>> {
    let mut out = HashMap::new();
    for (vip, f) in k.sent.lock().iter() {
        let e = peers_of(f);
        let marks = crew_marks(f).map(|m| m.1).unwrap_or_else(|| vec![0; e.len()]);
        out.insert(*vip, e.iter().zip(marks).map(|(p, m)| (p.vip, m)).collect());
    }
    out
}

/// what player `vip` gets cars from (entries not marked TX) and sends its own to (not marked RX)
fn gets(l: &[(u32, u8)]) -> Vec<u32> {
    l.iter().filter(|e| e.1 & TX == 0).map(|e| e.0).collect()
}
fn sends(l: &[(u32, u8)]) -> Vec<u32> {
    l.iter().filter(|e| e.1 & RX == 0).map(|e| e.0).collect()
}

/// n players on a grid `gap` m apart (row of 10), each its own links and core, the first one in the middle
fn room(o: RoamOptions, n: u32, gap: f64, who: impl Fn(u32) -> (u16, u8)) -> (Kit, Vec<[f64; 3]>) {
    let mut k = Kit::new(o);
    let mut at = Vec::new();
    for i in 0..n {
        let (core, links) = who(i);
        // the first in the middle of the grid, the others round it
        let c = if i == 0 {
            55
        } else if i <= 55 {
            i - 1
        } else {
            i
        };
        let p = [(c % 10) as f64 * gap + 0.25, (c / 10) as f64 * gap + 0.25, 0.0];
        k.from(W(i), &hello_with(100 + i, &HelloOpts { core, links, ..Default::default() }));
        k.from(W(i), &snap(100 + i, p));
        at.push(p);
    }
    k.hub.tick();
    (k, at)
}

fn nearest(at: &[[f64; 3]], i: usize, n: usize) -> Vec<u32> {
    let mut d: Vec<(f64, u32)> =
        (0..at.len()).filter(|&j| j != i).map(|j| ((at[j][0] - at[i][0]).hypot(at[j][1] - at[i][1]), j as u32)).collect();
    d.sort_by(|a, b| a.0.total_cmp(&b.0).then(a.1.cmp(&b.1)));
    d.into_iter().take(n).map(|e| W(e.1)).collect()
}

fn opts() -> RoamOptions {
    RoamOptions { view_m: 1e-9, ..RoamOptions::default() }
}

// a player who shows 64 among 99 who show 32 (all core 15): it gets the cars of its 64 nearest (before, its links
// were the ones the others had room for); none gets more than it shows, none sends to more than 64
#[test]
fn a_player_who_shows_64_among_players_who_show_32_gets_its_64_nearest() {
    for (name, o) in [("every pair", opts()), ("the grid", RoamOptions { exact_room: 1, ..opts() })] {
        let (k, at) = room(o, 100, 6.0, |i| (15, if i == 0 { 64 } else { 32 }));
        let l = lists_with_marks(&k);
        let mine = gets(&l[&W(0)]);
        let want = nearest(&at, 0, 64);
        let hit = want.iter().filter(|v| mine.contains(v)).count();
        assert_eq!(mine.len(), 64, "{name}: the 64-player gets {} cars", mine.len());
        assert!(hit >= 62, "{name}: {hit} of its 64 nearest");
        for i in 1..100 {
            let li = &l[&W(i)];
            assert!(gets(li).len() <= 32, "{name}: player {i} gets {}", gets(li).len());
            assert!(sends(li).len() <= 64, "{name}: player {i} sends to {}", sends(li).len());
            assert!(li.len() <= 96, "{name}: player {i}'s list of {}", li.len());
        }
        // each way as the other end's list has it: what one gets, the other sends
        for (&v, li) in &l {
            for &(q, m) in li {
                let back = l[&q].iter().find(|e| e.0 == v).map(|e| e.1);
                let want_back = match m & (RX | TX) {
                    RX => TX,
                    TX => RX,
                    _ => 0,
                };
                assert_eq!(back.map(|b| b & (RX | TX)), Some(want_back), "{name}: {v:x} -> {q:x} mark {m:#x}, back {back:?}");
            }
        }
    }
}

// phones (16) send their car to 64 at most too (one copy from the phone, the server's for the rest): a player who
// shows 64 among 100 phones gets its 64 nearest; no phone gets more than 16, or sends to more than 64
#[test]
fn a_phone_sends_to_64_at_most_and_gets_its_16() {
    let (k, at) = room(opts(), 101, 6.0, |i| (15, if i == 0 { 64 } else { 16 }));
    let l = lists_with_marks(&k);
    for i in 1..101 {
        assert!(sends(&l[&W(i)]).len() <= 64, "phone {i} sends to {}", sends(&l[&W(i)]).len());
        assert!(gets(&l[&W(i)]).len() <= 16, "phone {i} gets {}", gets(&l[&W(i)]).len());
    }
    let mine = gets(&l[&W(0)]);
    let hit = nearest(&at, 0, 64).iter().filter(|v| mine.contains(v)).count();
    assert!(hit >= 62, "{hit} of its 64 nearest");
}

// under the busy cap a player sends to its links only
#[test]
fn under_the_busy_cap_a_player_sends_to_its_links() {
    let mut k = Kit::new(RoamOptions { view_m: 1e-9, cap_down_ms: 1.0, ..RoamOptions::default() });
    *k.busy.lock() = 0.95;
    for i in 0..60u32 {
        k.from(W(i), &hello_with(100 + i, &HelloOpts { core: 15, links: if i == 0 { 64 } else { 16 }, ..Default::default() }));
        k.from(W(i), &snap(100 + i, [(i % 10) as f64 * 6.0 + 0.25, (i / 10) as f64 * 6.0 + 0.25, 0.0]));
    }
    k.step(10.0);
    k.hub.tick();
    let l = lists_with_marks(&k);
    let cap = k.lines.lock().iter().any(|l| l.contains("links a player"));
    assert!(cap, "the cap came down");
    for i in 1..60 {
        assert!(sends(&l[&W(i)]).len() <= 16, "player {i} sends to {} under the cap", sends(&l[&W(i)]).len());
    }
}

// a player of an older core (14) gets links both ways only, unmarked, and the ones of core 15 keep theirs with it
#[test]
fn an_older_core_keeps_its_links_both_ways() {
    let (k, _) = room(opts(), 100, 6.0, |i| if i % 3 == 0 { (14, 48) } else { (15, if i % 2 == 0 { 64 } else { 32 }) });
    let l = lists_with_marks(&k);
    let old: Vec<u32> = (0..100).filter(|i| i % 3 == 0).map(W).collect();
    for &v in &old {
        for &(q, m) in &l[&v] {
            assert_eq!(m & (RX | TX), 0, "an old core's list has a way mark");
            let back = l[&q].iter().find(|e| e.0 == v).expect("both ways");
            assert_eq!(back.1 & (RX | TX), 0, "a link with an old core is marked one way");
        }
    }
    // somebody's links went one way
    assert!(k.info("onewayLinks") > 0.0);
}

// two players alone: both ways, no marks
#[test]
fn two_alone_are_linked_both_ways() {
    let (k, _) = room(opts(), 2, 6.0, |_| (15, 64));
    let l = lists_with_marks(&k);
    assert_eq!(l[&W(0)], vec![(W(1), 0)]);
    assert_eq!(l[&W(1)], vec![(W(0), 0)]);
}

// every pair weighed (a small room) and the grid's walks (a large one) give the same lists, byte for byte, tick after
// tick as the players move
#[test]
fn every_pair_and_the_grid_give_the_same_one_way_links() {
    let run = |exact_room: usize| {
        let o = RoamOptions { exact_room, ..RoamOptions::default() };
        let mut k = Kit::new(o);
        let mut out = Vec::new();
        for t in 0..6 {
            k.clear();
            for i in 0..150u32 {
                let a = i as f64 * 0.7;
                let r = 20.0 + (i % 7) as f64 * 15.0;
                let w = if i % 4 == 0 { 0.0 } else { 0.05 * (1.0 + (i % 5) as f64) };
                let ang = a + w * t as f64 * 2.0;
                let p = [r * ang.cos() + 0.25, r * ang.sin() + 0.25, 0.0];
                let v = [-r * w * ang.sin(), r * w * ang.cos(), 0.0];
                let links = [16u8, 24, 32, 32, 48, 64][i as usize % 6];
                if t == 0 {
                    k.from(W(i), &hello_with(100 + i, &HelloOpts { core: if i % 9 == 0 { 14 } else { 15 }, links, ..Default::default() }));
                }
                k.from(W(i), &snap_moving(100 + i, p, v, ang + 1.5, 2));
            }
            k.hub.tick();
            let mut l: Vec<(u32, Vec<u8>)> = k.sent.lock().iter().map(|(v, f)| (*v, f[16 + 16..].to_vec())).collect();
            l.sort();
            out.push(l);
            k.step(500.0);
        }
        out
    };
    let every = run(1000);
    let grid = run(1);
    for (t, (a, b)) in every.iter().zip(&grid).enumerate() {
        assert_eq!(a.len(), b.len());
        for (x, y) in a.iter().zip(b) {
            assert_eq!(x, y, "tick {t}: player {:x}'s list", x.0);
        }
    }
}

// nobody moves: the links stay as they were, tick after tick
#[test]
fn links_stay_while_nobody_moves() {
    let (mut k, _) = room(opts(), 100, 6.0, |i| (15, [16u8, 32, 64][i as usize % 3]));
    let first = lists_with_marks(&k);
    for _ in 0..8 {
        k.step(500.0);
        k.clear();
        for i in 0..100u32 {
            let c = if i == 0 {
                55
            } else if i <= 55 {
                i - 1
            } else {
                i
            };
            k.from(W(i), &snap(100 + i, [(c % 10) as f64 * 6.0 + 0.25, (c / 10) as f64 * 6.0 + 0.25, 0.0]));
        }
        k.hub.tick();
    }
    let last = lists_with_marks(&k);
    let sorted = |l: &HashMap<u32, Vec<(u32, u8)>>| {
        let mut v: Vec<(u32, Vec<(u32, u8)>)> = l
            .iter()
            .map(|(a, b)| {
                (*a, {
                    let mut b = b.clone();
                    b.sort();
                    b
                })
            })
            .collect();
        v.sort();
        v
    };
    assert_eq!(sorted(&first), sorted(&last));
}

// 300 players on one spot, all of them 64 and the room's 32s and 16s round them: no list names more than 96 (a core
// takes RC_LIST_MAX), each list's ways agree with the other ends'
#[test]
fn no_list_is_longer_than_96() {
    let (k, _) = room(opts(), 300, 2.0, |i| (15, [64u8, 64, 32, 16][i as usize % 4]));
    let l = lists_with_marks(&k);
    for (v, li) in &l {
        assert!(li.len() <= 96, "{v:x}'s list of {}", li.len());
        for &(q, m) in li {
            let back = l[&q].iter().find(|e| e.0 == *v).map(|e| e.1 & (RX | TX));
            let want = match m & (RX | TX) {
                RX => TX,
                TX => RX,
                _ => 0,
            };
            assert_eq!(back, Some(want));
        }
    }
}

/// a room of `n` core-15 players of `links` on a grid round the start, plus two at `a` and `b` set up by `hello`
fn crowd_and_two(n: u32, links: u8, a: [f64; 3], b: [f64; 3], hello: impl Fn(u32) -> HelloOpts<'static>) -> HashMap<u32, Vec<(u32, u8)>> {
    let mut k = Kit::new(opts());
    for i in 0..n {
        k.from(W(i), &hello_with(100 + i, &HelloOpts { core: 15, links, ..Default::default() }));
        k.from(W(i), &snap(100 + i, [(i % 10) as f64 * 4.0 + 0.25, (i / 10) as f64 * 4.0 + 0.25, 0.0]));
    }
    for (x, p) in [(n, a), (n + 1, b)] {
        k.from(W(x), &hello_with(100 + x, &hello(x)));
        k.from(W(x), &snap(100 + x, p));
    }
    k.hub.tick();
    lists_with_marks(&k)
}

// two crew-mates 300 m apart, each among 100 players of core 15: linked both ways, unmarked, as before one-way links
#[test]
fn crew_mates_far_apart_are_linked_both_ways() {
    static CREW: [u32; 1] = [0x1234_5678];
    let l = crowd_and_two(100, 32, [20.25, 20.25, 0.0], [320.25, 20.25, 0.0], |_| HelloOpts {
        core: 15,
        links: 32,
        crews: Some(&CREW),
        flags: 0x0400,
        ..Default::default()
    });
    let (a, b) = (W(100), W(101));
    let ab = l[&a].iter().find(|e| e.0 == b).map(|e| e.1 & (RX | TX));
    let ba = l[&b].iter().find(|e| e.0 == a).map(|e| e.1 & (RX | TX));
    assert_eq!((ab, ba), (Some(0), Some(0)), "crew-mates: {ab:?} {ba:?}");
}

// two players in an outrun (each names the other), 400 m apart in a crowd: linked both ways
#[test]
fn an_outrun_s_two_are_linked_both_ways() {
    let l = crowd_and_two(100, 16, [20.25, 20.25, 0.0], [420.25, 20.25, 0.0], |x| HelloOpts {
        core: 15,
        links: 32,
        duel: if x == 100 { 201 } else { 200 },
        ..Default::default()
    });
    let (a, b) = (W(100), W(101));
    let ab = l[&a].iter().find(|e| e.0 == b).map(|e| e.1 & (RX | TX));
    let ba = l[&b].iter().find(|e| e.0 == a).map(|e| e.1 & (RX | TX));
    assert_eq!((ab, ba), (Some(0), Some(0)), "the outrun's pair: {ab:?} {ba:?}");
}

// a crowded meet: 150 players 3 m apart who show 32, 16 or 24, and one who shows 64 in the middle: the cars in the
// middle are wanted by more players than they send to, and it still gets 64 (the second round walks further)
#[test]
fn a_player_who_shows_64_at_a_crowded_meet_gets_64() {
    let (k, at) = room(opts(), 150, 3.0, |i| (15, if i == 0 { 64 } else { [32u8, 16, 24, 32][i as usize % 4] }));
    let l = lists_with_marks(&k);
    let mine = gets(&l[&W(0)]);
    assert_eq!(mine.len(), 64, "it gets {}", mine.len());
    let hit = nearest(&at, 0, 64).iter().filter(|v| mine.contains(v)).count();
    assert!(hit >= 48, "{hit} of its 64 nearest");
    for i in 1..150 {
        assert!(sends(&l[&W(i)]).len() <= 64);
    }
}
