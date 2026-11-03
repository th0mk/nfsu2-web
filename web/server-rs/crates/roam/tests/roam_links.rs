//! How many links each player gets: what its HELLO asks for (roam_core version 10: the friends' cars its game shows,
//! up to 48), the hub's `degree` at most; a core before version 10, which reads lists of 32 at most, gets 32.

mod kit;

use kit::*;
use nfsu2_roam::RoamOptions;

/// browser i's address, for more players than V's: 10.77.0.2 and on
#[allow(non_snake_case)]
fn W(i: u32) -> u32 {
    0x0A4D_0002 + i
}

/// the links of the player at W(i), as the hub's lists to it say
fn links_of(k: &Kit, i: u32) -> usize {
    k.lists().get(&format!("{}:47210", W(i))).map_or(0, |l| l.len())
}

// 100 players within 30 m: those that ask for 16 get 16, those of a core before version 10 (which say nothing) 32 at
// most, those of version 10 that say nothing too, and those that ask for 48 more than 32, 48 at most and 90 % of that on
// the whole (the nearest pairs first: a player whose farther candidates are full keeps fewer, as at 32)
#[test]
fn players_get_the_links_they_ask_for() {
    let mut k = Kit::new(RoamOptions { view_m: 1e-9, ..RoamOptions::default() });
    let ask = |i: u32| -> (u16, u8) {
        match i {
            0..10 => (10, 16),
            10..20 => (9, 0),
            20..25 => (10, 0),
            _ => (10, 48),
        }
    };
    for i in 0..100u32 {
        let (core, links) = ask(i);
        k.from(W(i), &hello_with(100 + i, &HelloOpts { core, links, ..Default::default() }));
        k.from(W(i), &snap(100 + i, [(i % 10) as f64 * 3.0 + 0.25, (i / 10) as f64 * 3.0 + 0.25, 0.0]));
    }
    k.hub.tick();
    let mut many = Vec::new();
    for i in 0..100u32 {
        let n = links_of(&k, i);
        match ask(i) {
            (10, 16) => assert_eq!(n, 16, "player {i}"),
            (_, 0) => assert!((29..=32).contains(&n), "player {i} ({:?}): {n}", ask(i)),
            _ => {
                assert!((33..=48).contains(&n), "player {i}: {n}");
                many.push(n);
            }
        }
    }
    assert!(many.iter().sum::<usize>() as f64 >= 0.9 * 48.0 * many.len() as f64, "{many:?}");
    // asking for more than the hub gives: the hub's most
    let mut k = Kit::new(RoamOptions { degree: 40, view_m: 1e-9, ..RoamOptions::default() });
    for i in 0..60u32 {
        k.from(W(i), &hello_with(100 + i, &HelloOpts { core: 10, links: 48, ..Default::default() }));
        k.from(W(i), &snap(100 + i, [(i % 10) as f64 * 3.0 + 0.25, (i / 10) as f64 * 3.0 + 0.25, 0.0]));
    }
    k.hub.tick();
    let n: Vec<usize> = (0..60).map(|i| links_of(&k, i)).collect();
    assert!(n.iter().all(|&x| x <= 40) && n.iter().sum::<usize>() >= 36 * 60, "the hub's degree (40) at most: {n:?}");
}

// a player that asks for fewer links later (its Players' cars shown turned down) gets fewer at the next tick
#[test]
fn a_player_that_asks_for_fewer_gets_fewer() {
    let mut k = Kit::new(RoamOptions { view_m: 1e-9, ..RoamOptions::default() });
    let hello = |k: &mut Kit, i: u32, links: u8| k.from(W(i), &hello_with(100 + i, &HelloOpts { core: 10, links, ..Default::default() }));
    for i in 0..60u32 {
        hello(&mut k, i, 48);
        k.from(W(i), &snap(100 + i, [(i % 10) as f64 * 3.0 + 0.25, (i / 10) as f64 * 3.0 + 0.25, 0.0]));
    }
    k.hub.tick();
    assert!(links_of(&k, 0) > 32, "{}", links_of(&k, 0));
    hello(&mut k, 0, 20);
    k.clear();
    k.hub.tick();
    assert_eq!(links_of(&k, 0), 20);
    assert!((1..60).all(|i| (33..=48).contains(&links_of(&k, i))), "the others keep more than 32");
}

// the pairs the server's counts read (nfsu2-lan paths.rs): each link once, the lower address first, the same links the
// lists name; a player gone takes its pairs with it
#[test]
fn link_pairs_are_the_links_once() {
    let mut k = Kit::new(RoamOptions { view_m: 1e-9, ..RoamOptions::default() });
    for i in 0..5u32 {
        k.from(W(i), &hello(100 + i));
        k.from(W(i), &snap(100 + i, [i as f64 * 3.0 + 0.25, 0.25, 0.0]));
    }
    k.hub.tick();
    let pairs = k.hub.link_pairs();
    assert_eq!(pairs.len(), 10, "five players near each other: every pair linked once, {pairs:x?}");
    assert!(pairs.iter().all(|&(a, b)| a < b));
    let listed: usize = (0..5).map(|i| links_of(&k, i)).sum();
    assert_eq!(listed, 2 * pairs.len(), "the lists name each link from both ends");
    k.hub.peer_gone(W(4));
    k.hub.tick();
    let pairs = k.hub.link_pairs();
    assert_eq!(pairs.len(), 6);
    assert!(pairs.iter().all(|&(a, b)| a != W(4) && b != W(4)));
}
