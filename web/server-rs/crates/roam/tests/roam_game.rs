//! The game in a HELLO (roam_core.h RC_HF_GAME, PLAN D-18): Most Wanted's pages say theirs after their crews,
//! Underground 2's say none (as every HELLO before the flag), and a hub takes the players of its own game only.
//! 1. the wire: a HELLO without the flag is Underground 2's; with it, the game after the crews' count and crews, and
//!    what follows it skipped; the flag without the crews' flag, without a crew count or without the game byte is no
//!    HELLO;
//! 2. the hub of each game: its own players join and are linked, the other game's are refused and counted
//!    (`foreign`), their SNAPs go nowhere, and a player whose HELLO turns out to be another game's is gone; a HELLO
//!    as Underground 2's pages of before send it (no game) joins Underground 2's hub, never Most Wanted's.

mod kit;

use kit::*;
use nfsu2_roam::{RoamMsg, RoamOptions, decode_roam};

/// the HELLO flag of a player that says its game (roam_core.h RC_HF_GAME)
const HF_GAME: u16 = 0x0800;
const UG2: u8 = 0;
const MW: u8 = 1;

/// a HELLO of `sender` in room "blue", as roam_core.c lays it out, with these flags; after its ids `tail` as it is
fn hello_tail(sender: u32, flags: u16, tail: &[u8]) -> Vec<u8> {
    let name = b"p";
    let mut p = vec![0u8; 13 + name.len() + 1];
    p[0..2].copy_from_slice(&8u16.to_le_bytes());
    p[2] = 1;
    p[3] = 2;
    p[4..6].copy_from_slice(&47210u16.to_le_bytes());
    p[6..8].copy_from_slice(&3u16.to_le_bytes());
    p[8..12].copy_from_slice(&room_hash("blue").to_le_bytes());
    p[12] = name.len() as u8;
    p[13..13 + name.len()].copy_from_slice(name);
    p[13 + name.len()] = 0;
    p.extend_from_slice(tail);
    dgram_flags(1, sender, flags, &p)
}
/// a HELLO of this game: Underground 2's as its pages send it (the crew flags, no crews, no game), another game's with
/// the game flag, a crew count of 0 and the game
fn hello_of(sender: u32, game: u8) -> Vec<u8> {
    if game == UG2 { hello_tail(sender, HF_CREW | HF_HUBCREW, &[]) } else { hello_tail(sender, HF_CREW | HF_HUBCREW | HF_GAME, &[0, game]) }
}
fn game_of(d: &[u8]) -> Option<u8> {
    match decode_roam(d) {
        Some(RoamMsg::Hello { game, .. }) => Some(game),
        _ => None,
    }
}

#[test]
fn the_wire() {
    assert_eq!(game_of(&hello_tail(7, 0, &[])), Some(UG2), "a HELLO of before");
    assert_eq!(game_of(&hello_of(7, UG2)), Some(UG2), "Underground 2's");
    assert_eq!(game_of(&hello_of(7, MW)), Some(MW), "Most Wanted's");
    let blue = room_hash("blue");
    match decode_roam(&hello_tail(7, HF_CREW | HF_GAME, &[1, 0x11, 0x22, 0x33, 0x44, MW, 0xAA, 0xBB])) {
        Some(RoamMsg::Hello { game, crews, room, .. }) => assert_eq!((game, crews, room), (MW, vec![0x44332211], blue)),
        m => panic!("the game after one crew, then more: {m:?}"),
    }
    // the flag without what it says
    assert_eq!(game_of(&hello_tail(7, HF_GAME, &[0, MW])), None, "without the crews' flag");
    assert_eq!(game_of(&hello_tail(7, HF_CREW | HF_GAME, &[])), None, "no crew count");
    assert_eq!(game_of(&hello_tail(7, HF_CREW | HF_GAME, &[0])), None, "no game");
    assert_eq!(game_of(&hello_tail(7, HF_CREW | HF_GAME, &[1, 1, 0, 0, 0])), None, "a crew, no game");
    // a hub of before reads Most Wanted's HELLO as a HELLO with no crews: the game byte is what it skips
    match decode_roam(&hello_tail(7, HF_CREW, &[0, MW])) {
        Some(RoamMsg::Hello { game, crews, .. }) => assert_eq!((game, crews.len()), (UG2, 0)),
        m => panic!("{m:?}"),
    }
}

/// the hub of `game`, without view points (the tests' cars point nowhere)
fn two_games(game: u8) -> Kit {
    Kit::new(RoamOptions { game, view_m: 1e-9, ..RoamOptions::default() })
}

#[test]
fn each_hub_takes_its_own_game() {
    for (hub, own, other) in [(UG2, UG2, MW), (MW, MW, UG2)] {
        let mut k = two_games(hub);
        // two players of the hub's game and one of the other game, all near each other
        for (i, g) in [(1u32, own), (2, own), (3, other)] {
            k.from(V(i), &hello_of(0x100 + i, g));
            k.from(V(i), &snap(0x100 + i, [i as f64 * 10.0, 0.0, 0.0]));
        }
        k.hub.tick();
        assert_eq!(k.info("players"), 2.0, "hub {hub}: its own two");
        assert_eq!(k.info("foreign"), 1.0, "hub {hub}: the other game's HELLO counted");
        let lists = k.lists();
        let all: Vec<u32> = lists.values().flatten().map(|e| e.id).collect();
        assert!(all.contains(&0x101) && all.contains(&0x102), "hub {hub}: its players are listed to each other ({all:x?})");
        assert!(!all.contains(&0x103), "hub {hub}: the other game's player is listed to nobody");
        assert!(!lists.keys().any(|key| key.starts_with(&format!("{}:", V(3)))), "hub {hub}: the other game's player gets no list");
        assert!(k.lines.lock().iter().any(|l| l.contains("another game")), "hub {hub}: one line says so");
        // a player of the hub's game whose next HELLO is the other game's is gone
        k.clear();
        k.from(V(2), &hello_of(0x102, other));
        assert_eq!(k.info("players"), 1.0, "hub {hub}: a player turned another game's is gone");
        assert_eq!(k.info("foreign"), 2.0);
        assert_eq!(k.lines.lock().iter().filter(|l| l.contains("another game")).count(), 1, "hub {hub}: one line, then counts");
    }
}

#[test]
fn a_page_of_before_joins_ug2_only() {
    // Underground 2's pages of before (and service-worker copies of them during a release): the crew flags, no crews,
    // no game; as the test kit's own HELLO, with no flags at all
    for d in [hello_tail(0x201, HF_CREW | HF_HUBCREW, &[]), hello(0x202)] {
        let mut ug2 = two_games(UG2);
        ug2.from(V(1), &d);
        assert_eq!((ug2.info("players"), ug2.info("foreign")), (1.0, 0.0), "Underground 2's hub takes it");
        let mut mw = two_games(MW);
        mw.from(V(1), &d);
        assert_eq!((mw.info("players"), mw.info("foreign")), (0.0, 1.0), "Most Wanted's hub refuses it");
    }
}
