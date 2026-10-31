//! Friends in free roam (the server's friends.rs): the server tells the hub each page's friends (their keyphrases),
//! and the hub links two friends of one room first, as two crew-mates, while each one's list names the other; a page
//! may ask to befriend only a player it is linked with; and its friends find where it plays.
//! 1. the links: a friend 2 km off a crowd is linked, in any way a room is linked; one whose list does not name the
//!    other back is not, nor a friend in another room, nor one without a keyphrase;
//! 2. linked_with: the player of that name (its Online form) that the page's player is linked with, its car within
//!    30 m, with its browser and keyphrase (if its page has one); none farther, linked or not;
//! 3. whereabouts: by keyphrase, the room and its name as the page told it; none for one not in free roam.

mod kit;

use std::sync::Arc;

use kit::*;
use nfsu2_claims::owner_of_token;
use nfsu2_roam::{RoamOptions, Whereabouts};

fn owner(c: char) -> Arc<str> {
    owner_of_token(&c.to_string().repeat(43)).unwrap()
}

/// player i's address, for more players than V's: 10.77.1.0 and on
#[allow(non_snake_case)]
fn U(i: u32) -> u32 {
    0x0A4D_0100 + i
}

/// a player named `name`, of keyphrase `key` (None: none), at (x, y) in `room`: its HELLO and SNAP
fn put(k: &mut Kit, vip: u32, id: u32, room: u32, name: &[u8], key: Option<char>, at: [f64; 2]) {
    let o = key.map(owner);
    k.hub.frame_from(vip, &frame(vip, &hello_with(id, &HelloOpts { core: 6, room, name, ..Default::default() })), o.as_ref());
    k.hub.frame_from(vip, &frame(vip, &snap(id, [at[0], at[1], 0.0])), o.as_ref());
}

fn linked(k: &Kit, a: u32, b: u32) -> bool {
    let l = k.lists();
    let has = |x: u32, y: u32| l.get(&format!("{x}:47210")).is_some_and(|v| v.iter().any(|e| e.vip == y));
    assert_eq!(has(a, b), has(b, a), "a link is listed at both ends");
    has(a, b)
}

fn ways() -> [(&'static str, RoamOptions, u32); 3] {
    let o = RoamOptions { view_m: 1e-9, ..RoamOptions::default() };
    [("every pair", o.clone(), 60), ("the grid", RoamOptions { exact_room: 1, ..o.clone() }, 60), ("a room past exactRoom", o, 300)]
}

/// A (keyphrase 'A') with `n` players packed around it, and B (keyphrase `b_key`) 2 km off, in `b_room`
fn crowd(k: &mut Kit, n: u32, b_key: Option<char>, b_room: u32) {
    put(k, U(0), 1000, 0, b"Alpha", Some('A'), [0.25, 0.25]);
    let side = (n as f64).sqrt().ceil() as u32;
    for i in 1..=n {
        let (x, y) = (((i - 1) % side) as f64 * 3.0, ((i - 1) / side) as f64 * 3.0);
        let half = (side - 1) as f64 * 1.5;
        put(k, U(i), 1000 + i, 0, format!("p{i}").as_bytes(), None, [x - half + 1.25, y - half + 1.25]);
    }
    put(k, U(n + 1), 1000 + n + 1, b_room, b"Bravo", b_key, [2000.25, 0.25]);
}

// a friend 2 km off a crowd is linked first while the friendship goes both ways and both are in one room
#[test]
fn a_friend_far_off_a_crowd_is_linked_first() {
    for (how, o, n) in ways() {
        let b = U(n + 1);
        for (a_lists_b, b_lists_a, b_key, b_room, want) in [
            (true, true, Some('B'), 0, true),
            (true, false, Some('B'), 0, false),
            (false, true, Some('B'), 0, false),
            (true, true, Some('B'), fnv1a("red"), false),
            (true, true, None, 0, false),
        ] {
            let what = format!("{how}: A lists B {a_lists_b}, B lists A {b_lists_a}, B's key {b_key:?}, room {b_room}");
            // (told before the players came: nobody is in free roam to be told back)
            let mut k = Kit::new(o.clone());
            if a_lists_b {
                k.hub.friends(U(0), owner('A'), vec![owner('B')]);
            }
            if b_lists_a {
                k.hub.friends(b, owner('B'), vec![owner('A'), owner('C')]);
            }
            crowd(&mut k, n, b_key, b_room);
            k.hub.tick();
            assert_eq!(linked(&k, U(0), b), want, "{what}");
            assert_eq!(k.lists()[&format!("{}:47210", U(0))].len(), 32, "{what}: A's links are full");
        }
        // one page's friends told once both play: the other one in free roam gets it back, linked from the next tick
        let mut k = Kit::new(o.clone());
        crowd(&mut k, n, Some('B'), 0);
        k.hub.tick();
        assert!(!linked(&k, U(0), b), "{how}");
        k.hub.friends(b, owner('B'), vec![owner('A')]);
        k.clear();
        k.hub.tick();
        assert!(linked(&k, U(0), b), "{how}: friends now");
    }
}

// linked_with: the linked player of that name, by its Online form, with a keyphrase
#[test]
fn a_page_befriends_only_a_player_it_is_linked_with() {
    let mut k = Kit::new(RoamOptions::default());
    put(&mut k, V(0), 100, 0, b"Alpha", Some('A'), [0.25, 0.25]);
    put(&mut k, V(1), 101, 0, b"Bravo Two", Some('B'), [3.25, 0.25]);
    put(&mut k, V(2), 102, 0, b"Charlie", None, [6.25, 0.25]);
    put(&mut k, V(3), 103, fnv1a("red"), b"Delta", Some('D'), [3.25, 3.25]);
    put(&mut k, V(4), 104, 0, b"Echo", Some('E'), [600.25, 0.25]);
    k.hub.tick();
    assert!(linked(&k, V(0), V(4)), "a room of five: linked, 600 m off");
    assert_eq!(k.hub.linked_with(V(0), "Echo"), None, "but its car far from this one's");
    assert_eq!(k.hub.linked_with(V(0), "Bravo_Two"), Some((V(1), Some(owner('B')))), "by its Online form");
    assert_eq!(k.hub.linked_with(V(0), "bravo two"), Some((V(1), Some(owner('B')))));
    assert_eq!(k.hub.linked_with(V(0), "Charlie"), Some((V(2), None)), "linked, without a keyphrase");
    assert_eq!(k.hub.linked_with(V(0), "Delta"), None, "another room: not linked");
    assert_eq!(k.hub.linked_with(V(0), "Alpha"), None, "itself");
    assert_eq!(k.hub.linked_with(V(9), "Bravo_Two"), None, "no player");
}

// whereabouts: where each keyphrase plays, with its room's name as its page told it
#[test]
fn friends_find_where_one_plays() {
    let mut k = Kit::new(RoamOptions::default());
    put(&mut k, V(0), 100, 0, b"Alpha", Some('A'), [0.25, 0.25]);
    put(&mut k, V(1), 101, fnv1a("blueroom"), b"Bravo", Some('B'), [3.25, 0.25]);
    k.hub.room_name(V(1), "Blue Room".into());
    k.hub.tick();
    let w = k.hub.whereabouts(&[owner('A'), owner('B'), owner('C')]);
    assert_eq!(
        w,
        [
            Some(Whereabouts { room: 0, room_name: None }),
            Some(Whereabouts { room: fnv1a("blueroom"), room_name: Some("Blue Room".into()) }),
            None
        ]
    );
    // gone from the switch: nowhere, its room's name forgotten
    k.hub.peer_gone(V(1));
    k.hub.tick();
    assert_eq!(k.hub.whereabouts(&[owner('B')]), [None]);
}
