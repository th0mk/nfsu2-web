//! Outruns between players (runtime/roam_game.c du_*, roam_core.c rc_set_duel): two players who name each other as
//! their outrun's other player in their HELLOs (core version 11) are linked before anyone else, however many players
//! are nearer to either; one player's word alone links nobody; the link goes with the outrun.  By distance alone the
//! one who fell behind went from both screens in a crowd, and both games won.

mod kit;

use kit::*;
use nfsu2_roam::RoamOptions;

/// the hub's last list to the player at V(i) names the one at V(j)
fn linked(k: &Kit, i: u32, j: u32) -> bool {
    k.lists().get(&format!("{}:47210", V(i))).is_some_and(|l| l.iter().any(|e| e.vip == V(j)))
}

fn links_of(k: &Kit, i: u32) -> usize {
    k.lists().get(&format!("{}:47210", V(i))).map_or(0, |l| l.len())
}

/// A (V(0), id 100) at the origin and B (V(1), id 101) 100 m off, each naming its outrun's other player (0: none), and 60
/// players within 12 m round B (ids 102..): every one of B's 32 nearest is nearer than A, and some 30 of A's are nearer
/// than B.  Everyone's HELLO and SNAP again (a tick later on, after gone_ms, still sees them all).
fn crowd(k: &mut Kit, duel_a: u32, duel_b: u32) {
    k.from(V(0), &hello_with(100, &HelloOpts { core: 11, links: 32, duel: duel_a, ..Default::default() }));
    k.from(V(0), &snap(100, [0.25, 0.25, 0.0]));
    k.from(V(1), &hello_with(101, &HelloOpts { core: 11, links: 32, duel: duel_b, name: b"Bob", ..Default::default() }));
    k.from(V(1), &snap(101, [100.25, 0.25, 0.0]));
    for i in 2..62u32 {
        k.from(V(i), &hello_with(100 + i, &HelloOpts { core: 10, links: 32, ..Default::default() }));
        k.from(V(i), &snap(100 + i, [100.25 + ((i - 2) % 8) as f64 * 3.0 - 10.5, ((i - 2) / 8) as f64 * 3.0 - 10.5, 0.0]));
    }
}

// the two who name each other are linked, the crowd nearer to both however (every pair | the grid); without the
// outrun they are not; each still has its 32 links at most, both ends list each other
#[test]
fn an_outruns_two_players_are_linked_first_whatever_the_crowd() {
    for (how, o) in ways() {
        let mut k = Kit::new(o.clone());
        crowd(&mut k, 0, 0);
        k.hub.tick();
        assert!(!linked(&k, 0, 1) && !linked(&k, 1, 0), "{how}: no outrun, the crowd's nearer: not linked");
        assert_eq!(k.info("duelLinks"), 0.0);

        let mut k = Kit::new(o.clone());
        crowd(&mut k, 101, 100);
        k.hub.tick();
        assert!(linked(&k, 0, 1) && linked(&k, 1, 0), "{how}: the outrun's two linked, both ends");
        // (A, out of the crowd, gets what the crowd's own leave: a few)
        assert!(
            links_of(&k, 0) <= 32 && links_of(&k, 1) == 32,
            "{how}: 32 links at most, the outrun's one of them ({}, {})",
            links_of(&k, 0),
            links_of(&k, 1)
        );
        assert_eq!(k.info("duelLinks"), 1.0, "{how}");
        // under the busy cap (8 links a player): still linked
        *k.busy.lock() = 0.95;
        for _ in 0..12 {
            k.step(5_000.0);
            crowd(&mut k, 101, 100);
            k.clear();
            k.hub.tick();
        }
        assert!(k.info("cap") <= 8.0, "{how}: the busy cap down ({})", k.info("cap"));
        assert!(linked(&k, 0, 1) && linked(&k, 1, 0) && links_of(&k, 1) <= 8, "{how}: under the busy cap, still linked");
    }
}

// one player's word links nobody: A names B, and B names nobody, or another player (who names nobody); a far player
// who names A back is linked, 2 km off
#[test]
fn one_players_word_alone_links_nobody() {
    for (how, o) in ways() {
        for b in [0, 102] {
            let mut k = Kit::new(o.clone());
            crowd(&mut k, 101, b);
            k.hub.tick();
            assert!(!linked(&k, 0, 1), "{how}: A names B, B names {b}: not linked");
            assert_eq!(k.info("duelLinks"), 0.0, "{how}");
        }
        let mut k = Kit::new(o.clone());
        crowd(&mut k, 162, 0);
        k.from(V(62), &hello_with(162, &HelloOpts { core: 11, links: 32, duel: 100, ..Default::default() }));
        k.from(V(62), &snap(162, [2000.25, 0.25, 0.0]));
        k.hub.tick();
        assert!(linked(&k, 0, 62) && linked(&k, 62, 0), "{how}: the two naming each other, 2 km apart: linked");
    }
}

// the link goes with the outrun: once neither names the other, it is kept as any link (hold_ms young, then at `keep`
// its distance) and goes for the crowd
#[test]
fn the_link_goes_with_the_outrun() {
    for (how, o) in ways() {
        let mut k = Kit::new(o.clone());
        crowd(&mut k, 101, 100);
        k.hub.tick();
        assert!(linked(&k, 0, 1), "{how}: linked in the outrun");
        k.step(1_000.0);
        crowd(&mut k, 0, 0);
        k.clear();
        k.hub.tick();
        assert!(linked(&k, 0, 1), "{how}: a second on, a young link: kept");
        assert_eq!(k.info("duelLinks"), 0.0, "{how}");
        k.step(o.hold_ms);
        crowd(&mut k, 0, 0);
        k.clear();
        k.hub.tick();
        assert!(!linked(&k, 0, 1) && !linked(&k, 1, 0), "{how}: past hold_ms, the crowd's nearer: gone");
    }
}

// a pair of the outrun kept apart (A blocks B's name) is no link: the blocks come first
#[test]
fn a_blocked_player_is_no_outruns_link() {
    let mut k = Kit::new(RoamOptions { view_m: 1e-9, ..RoamOptions::default() });
    k.hub.blocks(V(0), vec!["bob".into()]);
    crowd(&mut k, 101, 100);
    k.hub.tick();
    assert!(!linked(&k, 0, 1) && !linked(&k, 1, 0), "kept apart");
}
