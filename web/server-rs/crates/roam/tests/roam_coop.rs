//! Co-op free roam through the hub: a browser's second local player is a second endpoint of its browser (player 1's
//! port + 1), its HELLO flagged (roam_core.h RC_HF_SEAT2), its name the second form of player 1's ("Alex 2").
//! 1. the links: two endpoints of one browser are never linked, by any way the hub links (the nearest, every pair or
//!    the grid, an outrun's, a race lobby's, a crew's, friends), flagged or not; everyone else links with both;
//! 2. the maps: a browser's world map and minimap leave out its other endpoints; a second player's questions go
//!    unanswered (the browser's answer state is its first player's), and the second player is on everyone else's;
//! 3. friends: the second player is nobody's friend (no marks either way, no friend linked first), and a keyphrase's
//!    whereabouts are its first player's;
//! 4. the console: a second player under the second form of its page's name, or of its claim, is a line of its own,
//!    not "its game shows"; the operator's `who` still has its name;
//! 5. the cheat watch: a second player's car put beside its first one's is no teleport; anywhere else it is, and so
//!    is one beside another flagged endpoint or beside an endpoint not flagged;
//! 6. a block of the second player's name, as a claimed name's, blocks its owner (both players); a second form two
//!    claims of before share is one keyphrase's: another one's player under it is an impostor, and a block of it never
//!    blocks that other keyphrase;
//! 7. the counts: /players counts the browser once, info() its second players; a full hub of 15360 with players
//!    coming and going keeps every slot a world map's key can name.

mod kit;

use std::collections::{HashMap, HashSet};
use std::sync::Arc;

use kit::*;
use nfsu2_claims::{Claims, owner_of_token};
use nfsu2_roam::{PeerEntry, RoamOptions};

/// roam_core.h RC_HF_SEAT2: this endpoint is its browser's second local player
const SEAT2: u16 = 0x2000;
/// player 1's port and player 2's
const P1: u16 = 47210;
const P2: u16 = 47211;

fn owner(c: char) -> Arc<str> {
    owner_of_token(&c.to_string().repeat(43)).unwrap()
}

/// a datagram from browser `vip`'s endpoint at `port`, whose page plays with `key`'s keyphrase (None: none)
fn send(k: &mut Kit, vip: u32, port: u16, d: &[u8], key: Option<char>) {
    let o = key.map(owner);
    k.hub.frame_from(vip, &frame_ports(vip, d, port, 47210), o.as_ref());
}

/// a player with these HELLO fields at (x, y): its HELLO and SNAP from browser `vip`'s endpoint `port`
fn put(k: &mut Kit, vip: u32, port: u16, id: u32, h: &HelloOpts, key: Option<char>, at: [f64; 2]) {
    send(k, vip, port, &hello_with(id, h), key);
    send(k, vip, port, &snap(id, [at[0], at[1], 0.0]), key);
}

/// player 2's HELLO: flagged, no crews (the crew flag and a count of 0, so that the links byte is read), few links
fn seat2<'a>(name: &'a [u8]) -> HelloOpts<'a> {
    HelloOpts { core: 12, name, flags: SEAT2 | HF_HUBCREW, crews: Some(&[]), links: 8, ..Default::default() }
}

/// player 1's HELLO, of a core of now
fn seat1<'a>(name: &'a [u8]) -> HelloOpts<'a> {
    HelloOpts { core: 12, name, flags: HF_HUBCREW, crews: Some(&[]), links: 24, ..Default::default() }
}

fn ep(vip: u32, port: u16) -> String {
    format!("{vip}:{port}")
}

/// whether endpoint a's list names endpoint b (and b's names a: a link is listed at both ends)
fn linked(l: &HashMap<String, Vec<PeerEntry>>, a: (u32, u16), b: (u32, u16)) -> bool {
    let has = |x: (u32, u16), y: (u32, u16)| l.get(&ep(x.0, x.1)).is_some_and(|v| v.iter().any(|e| (e.vip, e.port) == y));
    assert_eq!(has(a, b), has(b, a), "{a:?} {b:?}: a link is listed at both ends");
    has(a, b)
}

/// the MAPD and NEARD datagrams the hub sent since the last clear, to which endpoint
fn answers(k: &Kit) -> Vec<((u32, u16), Mapd, bool)> {
    let sent = k.sent.lock();
    let to = |vip: u32, f: &[u8]| (vip, u16::from_be_bytes([f[14], f[15]]));
    let maps = sent.iter().filter_map(|(vip, f)| mapd_of(f).map(|m| (to(*vip, f), m, false)));
    let nears = sent.iter().filter_map(|(vip, f)| neard_of(f).map(|m| (to(*vip, f), m, true)));
    maps.chain(nears).collect()
}

/// the x of each player placed in an answer
fn xs(m: &Mapd) -> HashSet<i16> {
    m.5.iter().filter_map(|e| e.1.map(|p| p.0)).collect()
}

// a browser's two endpoints, player 1 and player 2 three metres apart among three others: everyone is linked with both
// of them, they never with each other; nor two endpoints of a browser without the flag (a restart's lingering one)
#[test]
fn a_browsers_endpoints_are_never_linked() {
    for (how, o) in ways() {
        let mut k = Kit::new(o);
        put(&mut k, V(0), P1, 100, &seat1(b"Alex"), Some('A'), [0.25, 0.25]);
        put(&mut k, V(0), P2, 200, &seat2(b"Alex 2"), Some('A'), [3.25, 0.25]);
        for i in 1..=3 {
            put(&mut k, V(i), P1, 100 + i, &seat1(format!("p{i}").as_bytes()), None, [3.0 * i as f64 + 3.25, 0.25]);
        }
        put(&mut k, V(4), P1, 104, &seat1(b"Restart"), None, [20.25, 0.25]);
        put(&mut k, V(4), 47212, 204, &seat1(b"Restart"), None, [20.25, 1.25]);
        k.hub.tick();
        let l = k.lists();
        assert!(!linked(&l, (V(0), P1), (V(0), P2)), "{how}: player 1 and player 2");
        assert!(!linked(&l, (V(4), P1), (V(4), 47212)), "{how}: a browser's endpoints without the flag");
        for i in 1..=3 {
            assert!(linked(&l, (V(0), P1), (V(i), P1)) && linked(&l, (V(0), P2), (V(i), P1)), "{how}: {i} with both");
        }
        assert!(linked(&l, (V(4), P1), (V(0), P2)) && linked(&l, (V(4), 47212), (V(0), P2)), "{how}: two browsers' endpoints");
        assert_eq!(k.hub.link_pairs().len(), 10, "{how}: every pair of the five browsers");
    }
}

// the links made before any other are none of them either: the two in one crew, in an outrun with each other, player
// 1 hosting a race lobby with player 2 at its start
#[test]
fn a_browsers_endpoints_are_linked_first_by_nothing() {
    let owls = fnv1a("owls");
    for (how, o) in ways() {
        let mut k = Kit::new(o);
        let one = HelloOpts { crews: Some(&[owls]), duel: 200, ..seat1(b"Alex") };
        let two = HelloOpts { crews: Some(&[owls]), duel: 100, ..seat2(b"Alex 2") };
        put(&mut k, V(0), P1, 100, &one, Some('A'), [0.25, 0.25]);
        put(&mut k, V(0), P2, 200, &two, Some('A'), [3.25, 0.25]);
        // a crew-mate 2 km off and a player beside them
        put(&mut k, V(1), P1, 101, &HelloOpts { crews: Some(&[owls]), ..seat1(b"Owl") }, None, [2000.25, 0.25]);
        put(&mut k, V(2), P1, 102, &seat1(b"Near"), None, [6.25, 0.25]);
        // player 1's lobby, its start where it is (an advert: op 1, max 4, lobby a1, race, x 0, y 0, n 1, its id)
        let mut p = vec![0u8; 28];
        p[0] = 1;
        p[3] = 4;
        p[8..12].copy_from_slice(&0xA1u32.to_le_bytes());
        p[12..16].copy_from_slice(&0x0BAD_F00Du32.to_le_bytes());
        p[22] = 1;
        p[24..28].copy_from_slice(&100u32.to_le_bytes());
        send(&mut k, V(0), P1, &dgram(12, 100, &p), Some('A'));
        k.hub.tick();
        let l = k.lists();
        assert!(!linked(&l, (V(0), P1), (V(0), P2)), "{how}");
        assert!(linked(&l, (V(0), P1), (V(1), P1)), "{how}: player 1's crew-mate far off");
        assert!(linked(&l, (V(0), P1), (V(2), P1)) && linked(&l, (V(0), P2), (V(2), P1)), "{how}");
        assert_eq!(k.info("duelLinks"), 0.0, "{how}");
    }
}

// a browser's world map and minimap leave out its other endpoints (its game shows their cars itself); player 2's
// questions go unanswered, and take nothing of player 1's answers; player 2 is on another player's map and minimap
#[test]
fn a_browsers_maps_are_its_first_players() {
    let mut k = Kit::new(RoamOptions { degree: 1, view_m: 1e-9, ..RoamOptions::default() });
    let plain = |name: &'static [u8]| HelloOpts { core: 4, name, ..Default::default() };
    put(&mut k, V(0), P1, 100, &plain(b"Alex"), None, [0.25, 0.25]);
    put(&mut k, V(0), P2, 200, &HelloOpts { flags: SEAT2, ..plain(b"Alex 2") }, None, [3.25, 0.25]);
    for i in 1..=3 {
        put(&mut k, V(i), P1, 100 + i, &plain(b"p"), None, [100.0 * i as f64 + 0.25, 0.25]);
    }
    // (a degree of 1: player 2 with 1, 2 with 3, player 1 with nobody)
    k.hub.tick();
    let l = k.lists();
    assert!(linked(&l, (V(0), P2), (V(1), P1)) && linked(&l, (V(2), P1), (V(3), P1)));
    assert!(l[&ep(V(0), P1)].is_empty());
    k.clear();
    // player 2 asks first: nothing; player 1 at once after it: its answers
    send(&mut k, V(0), P2, &mapreq_base(200, 1, 64, 0), None);
    send(&mut k, V(0), P2, &nearreq(200, 1, 64, 0, 1000), None);
    assert!(answers(&k).is_empty(), "player 2's questions unanswered");
    send(&mut k, V(0), P1, &mapreq_base(100, 2, 64, 0), None);
    send(&mut k, V(0), P1, &nearreq(100, 3, 64, 0, 1000), None);
    let a = answers(&k);
    assert_eq!(a.len(), 2, "player 1's map and minimap: {a:?}");
    for (to, m, near) in &a {
        assert_eq!(*to, (V(0), P1), "near {near}");
        assert_eq!(xs(m), HashSet::from([100, 200, 300]), "near {near}: the others, not player 2 (x 3)");
    }
    // another player's: player 2 is on them (3's: past its link, 2)
    k.clear();
    send(&mut k, V(3), P1, &mapreq_base(103, 1, 64, 0), None);
    send(&mut k, V(3), P1, &nearreq(103, 2, 64, 0, 1000), None);
    for (_, m, near) in answers(&k) {
        assert_eq!(xs(&m), HashSet::from([0, 3, 100]), "near {near}: both of the browser's players");
    }
}

// player 2 is nobody's friend: player 1's friend sees player 1 marked and player 2 not, player 2's list marks nobody,
// and player 1's friend 2 km off is linked first with player 1 only; the keyphrase's whereabouts are player 1's
#[test]
fn a_second_player_is_nobodys_friend() {
    let o = RoamOptions { degree: 2, view_m: 1e-9, ..RoamOptions::default() };
    let mut k = Kit::new(o);
    k.hub.friends(V(0), owner('A'), vec![owner('B')]);
    k.hub.friends(V(1), owner('B'), vec![owner('A')]);
    let two = |name| HelloOpts { links: 2, ..seat2(name) };
    let one = |name| HelloOpts { links: 2, ..seat1(name) };
    put(&mut k, V(0), P1, 100, &one(b"Alex"), Some('A'), [0.25, 0.25]);
    put(&mut k, V(0), P2, 200, &two(b"Alex 2"), Some('A'), [3.25, 0.25]);
    put(&mut k, V(1), P1, 101, &one(b"Bravo"), Some('B'), [2000.25, 0.25]);
    for i in 2..=5 {
        put(&mut k, V(i), P1, 100 + i, &one(b"x"), None, [i as f64 + 3.25, 0.25]);
    }
    k.hub.tick();
    let l = k.lists();
    assert!(linked(&l, (V(0), P1), (V(1), P1)), "player 1's friend far off: linked first");
    assert!(!linked(&l, (V(0), P2), (V(1), P1)), "player 2's links are by distance");
    // the marks: Bravo near both of them
    let mut k = Kit::new(RoamOptions::default());
    k.hub.friends(V(0), owner('A'), vec![owner('B')]);
    k.hub.friends(V(1), owner('B'), vec![owner('A')]);
    put(&mut k, V(0), P1, 100, &seat1(b"Alex"), Some('A'), [0.25, 0.25]);
    put(&mut k, V(0), P2, 200, &seat2(b"Alex 2"), Some('A'), [3.25, 0.25]);
    put(&mut k, V(1), P1, 101, &seat1(b"Bravo"), Some('B'), [1.25, 1.25]);
    k.hub.tick();
    let sent = k.sent.lock().clone();
    let list = |vip: u32, port: u16| {
        let f = sent.iter().find(|(v, f)| *v == vip && u16::from_be_bytes([f[14], f[15]]) == port).unwrap().1.clone();
        let m = crew_marks(&f);
        peers_of(&f).iter().enumerate().map(|(at, e)| ((e.vip, e.port), m.as_ref().map_or(0, |m| m.1[at]))).collect::<HashMap<_, _>>()
    };
    let bravo = list(V(1), P1);
    assert_eq!((bravo[&(V(0), P1)], bravo[&(V(0), P2)]), (MARK_FRIEND, 0), "Bravo's list: player 1 a friend, player 2 not");
    assert_eq!(list(V(0), P1)[&(V(1), P1)], MARK_FRIEND, "player 1's: Bravo");
    assert!(list(V(0), P2).values().all(|&m| m == 0), "player 2's: nobody");
    // whereabouts: player 1's room, player 2 in another
    let red = fnv1a("red");
    put(&mut k, V(0), P2, 200, &HelloOpts { room: red, ..seat2(b"Alex 2") }, Some('A'), [3.25, 0.25]);
    k.hub.tick();
    assert_eq!(k.hub.whereabouts(&[owner('A')])[0].as_ref().map(|w| w.room), Some(0));
}

// the console: a second player under the second form of its page's name (or of its keyphrase's claim) is its own line;
// a game that shows another name, a name like it without the flag, or the flag with another name: "its game shows"
#[test]
fn a_known_second_player_is_a_line_of_its_own() {
    let claims = Arc::new(Claims::in_memory());
    claims.claim("Charlie", &owner('C'), 1_000_000).unwrap();
    let mut k = Kit::new(RoamOptions::default());
    k.hub.set_claims(Some(claims));
    let pages = [
        (0, "Alex", "Alex 2", true),
        (1, "Bob", "Bob 2", false),
        (2, "Dan", "Mallory", true),
        (3, "Alex the Great", "Alex_the_Grea 2", true),
    ];
    for (i, page, p2, flag) in pages {
        k.hub.page_name(V(i), page.into());
        put(&mut k, V(i), P1, 100 + i, &seat1(page.as_bytes()), None, [0.25, 0.25]);
        let h = HelloOpts { flags: if flag { SEAT2 } else { 0 }, ..seat2(p2.as_bytes()) };
        put(&mut k, V(i), P2, 200 + i, &h, None, [3.25, 0.25]);
    }
    // its page goes by digits, its keyphrase's claimed name is Charlie
    k.hub.page_name(V(4), "Charlie1234".into());
    put(&mut k, V(4), P1, 104, &seat1(b"Charlie1234"), Some('C'), [0.25, 0.25]);
    put(&mut k, V(4), P2, 204, &seat2(b"Charlie 2"), Some('C'), [3.25, 0.25]);
    // and again: told once
    put(&mut k, V(0), P2, 200, &seat2(b"Alex 2"), None, [3.25, 0.25]);
    let lines = k.lines.lock().clone();
    let coop: Vec<&String> = lines.iter().filter(|l| l.contains("(co-op)")).collect();
    assert_eq!(
        coop,
        [
            "[roam] 10.77.0.10 \"Alex\": its second player (co-op) shows \"Alex 2\" over its car",
            "[roam] 10.77.0.13 \"Alex the Great\": its second player (co-op) shows \"Alex_the_Grea 2\" over its car",
            "[roam] 10.77.0.14 \"Charlie1234\": its second player (co-op) shows \"Charlie 2\" over its car",
        ]
    );
    let shows: Vec<&String> = lines.iter().filter(|l| l.contains("its game shows")).collect();
    assert_eq!(
        shows,
        [
            "[roam] 10.77.0.11 \"Bob\": its game shows \"Bob 2\" over its car",
            "[roam] 10.77.0.12 \"Dan\": its game shows \"Mallory\" over its car",
        ]
    );
    // who: the second players' names, as every other name its game shows
    let who = k.hub.other_names();
    assert_eq!(who[&V(0)], ["Alex 2"]);
    assert_eq!(who[&V(4)], ["Charlie 2"]);
}

/// Ann (V(0) player 1) parked at (1000, 0, 0) and a second endpoint of hers (`flags`, `port`) at the origin, a SNAP of
/// each 500 ms ago; Bob (V(1)) far off
fn ann_and(port: u16, flags: u16) -> Kit {
    let mut k = Kit::new(RoamOptions::default());
    let h = |name: &'static [u8], flags| HelloOpts { core: 12, room: fnv1a("bay"), name, flags, ..Default::default() };
    send(&mut k, V(0), P1, &hello_with(100, &h(b"Ann", 0)), None);
    send(&mut k, V(0), port, &hello_with(200, &h(b"Ann 2", flags)), None);
    send(&mut k, V(1), P1, &hello_with(101, &h(b"Bob", 0)), None);
    send(&mut k, V(0), P1, &snap(100, [1000.0, 0.0, 0.0]), None);
    send(&mut k, V(0), port, &snap(200, [0.0, 0.0, 0.0]), None);
    send(&mut k, V(1), P1, &snap(101, [-3000.0, 0.0, 0.0]), None);
    k.step(500.0);
    send(&mut k, V(0), P1, &snap(100, [1000.0, 0.0, 0.0]), None);
    k
}

fn cheats(k: &Kit) -> Vec<String> {
    k.lines.lock().iter().filter(|l| l.starts_with("[cheat]")).cloned().collect()
}

// player 2's car put beside player 1's, 1 km from where it was: no teleport, and none adds up from it; 2 km on from
// there, beside nobody, a teleport; and a jump beside player 1 of an endpoint without the flag, one 100 m off it, and
// one beside another flagged endpoint (no player 1), are teleports as ever
#[test]
fn a_second_player_put_beside_its_first_is_no_teleport() {
    let mut k = ann_and(P2, SEAT2);
    send(&mut k, V(0), P2, &snap(200, [1010.0, 5.0, 0.0]), None);
    k.step(500.0);
    send(&mut k, V(0), P2, &snap(200, [1015.0, 5.0, 0.0]), None);
    assert_eq!(cheats(&k), Vec::<String>::new());
    k.step(500.0);
    send(&mut k, V(0), P2, &snap(200, [3015.0, 5.0, 0.0]), None);
    let l = cheats(&k);
    assert!(l.len() == 1 && l[0].starts_with("[cheat] 10.77.0.10 \"Ann 2\": teleport 1: 2.0 km"), "{l:?}");
    // the same jump of an endpoint without the flag; one 100 m off player 1
    for (port, flags, to) in [(47212, 0, [1010.0, 5.0, 0.0]), (P2, SEAT2, [1100.0, 0.0, 0.0])] {
        let mut k = ann_and(port, flags);
        send(&mut k, V(0), port, &snap(200, to), None);
        let l = cheats(&k);
        assert!(l.len() == 1 && l[0].starts_with("[cheat] 10.77.0.10 \"Ann 2\": teleport 1"), "port {port}: {l:?}");
    }
    // two flagged endpoints: neither is a first player
    let mut k = ann_and(P2, SEAT2);
    let h = HelloOpts { core: 12, room: fnv1a("bay"), name: b"Ann", flags: SEAT2, ..Default::default() };
    send(&mut k, V(0), P1, &hello_with(100, &h), None);
    send(&mut k, V(0), P2, &snap(200, [1010.0, 5.0, 0.0]), None);
    assert_eq!(cheats(&k).len(), 1, "{:?}", cheats(&k));
}

// another page blocks player 2 by its name: as a claimed name's, its owner is blocked, both players (and player 1's
// name blocks player 2, its keyphrase's)
#[test]
fn a_block_of_a_second_players_name_blocks_its_owner() {
    let claims = Arc::new(Claims::in_memory());
    claims.claim("Alex", &owner('A'), 1_000_000).unwrap();
    for blocked in ["Alex 2", "Alex"] {
        let mut k = Kit::new(RoamOptions::default());
        k.hub.set_claims(Some(claims.clone()));
        k.hub.blocks(V(1), vec![blocked.to_string()]);
        put(&mut k, V(0), P1, 100, &seat1(b"Alex"), Some('A'), [0.25, 0.25]);
        put(&mut k, V(0), P2, 200, &seat2(b"Alex 2"), Some('A'), [3.25, 0.25]);
        put(&mut k, V(1), P1, 101, &seat1(b"Blocker"), Some('B'), [6.25, 0.25]);
        put(&mut k, V(2), P1, 102, &seat1(b"Other"), None, [9.25, 0.25]);
        k.hub.tick();
        let l = k.lists();
        assert!(!linked(&l, (V(1), P1), (V(0), P1)) && !linked(&l, (V(1), P1), (V(0), P2)), "{blocked} blocked");
        assert!(linked(&l, (V(2), P1), (V(0), P1)) && linked(&l, (V(2), P1), (V(0), P2)), "{blocked}: the others see both");
    }
}

// The 2026-11-05 security audit's case: "Speedracer123" (V) and "Speedracer123x" (A), claims of before that share one
// second form, "Speedracer123 2" (a claim of the second now is refused: nfsu2-claims).  It is V's only, the holder of
// the name of 13: A's player under it is an impostor, linked with nobody, and a block of it blocks V (its second
// player's name), never A's player under A's own claim.
#[test]
fn a_second_form_two_claims_share_is_one_keyphrases() {
    let t0 = 1_000_000;
    let dir = std::env::temp_dir().join(format!("nfsu2-roam-coop-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let claim = |name: &str, c: char, made: i64| format!(r#"{{"name":"{name}","owner":"{}","made":{made},"seen":{made}}}"#, owner(c));
    let file = format!(r#"{{"v":1,"claims":[{},{}]}}"#, claim("Speedracer123x", 'A', t0), claim("Speedracer123", 'V', t0 + 1));
    std::fs::write(dir.join("names.json"), file).unwrap();
    let claims = Arc::new(Claims::open(&dir, t0 + 2));
    std::fs::remove_dir_all(&dir).unwrap();
    assert_eq!(claims.count(t0 + 2), 2);
    for block in [false, true] {
        let mut k = Kit::new(RoamOptions::default());
        k.hub.set_claims(Some(claims.clone()));
        if block {
            k.hub.blocks(V(0), vec!["Speedracer123 2".into()]);
        }
        put(&mut k, V(0), P1, 100, &seat1(b"Watcher"), None, [0.25, 0.25]);
        put(&mut k, V(1), P1, 101, &seat1(b"Speedracer123"), Some('V'), [10.25, 0.25]);
        put(&mut k, V(2), P1, 102, &seat1(b"Speedracer123x"), Some('A'), [20.25, 0.25]);
        put(&mut k, V(3), P1, 103, &seat1(b"Speedracer123 2"), Some('A'), [30.25, 0.25]);
        k.hub.tick();
        let l = k.lists();
        assert!(!linked(&l, (V(0), P1), (V(3), P1)), "block {block}: A under V's second form, an impostor");
        assert!(linked(&l, (V(0), P1), (V(2), P1)), "block {block}: A under its own claim");
        assert_eq!(linked(&l, (V(0), P1), (V(1), P1)), !block, "block {block}: V, the second form's owner");
    }
}

// /players counts a browser once, its second player too; info() counts the second players in free roam
#[test]
fn a_second_player_counts_as_its_browser() {
    let mut k = Kit::new(RoamOptions::default());
    put(&mut k, V(0), P1, 100, &seat1(b"Alex"), None, [0.25, 0.25]);
    put(&mut k, V(0), P2, 200, &seat2(b"Alex 2"), None, [3.25, 0.25]);
    put(&mut k, V(1), P1, 101, &seat1(b"Bravo"), None, [6.25, 0.25]);
    assert_eq!((k.hub.in_roam(), k.info("players"), k.info("coopSeats")), (2, 3.0, 1.0));
    // player 2 in a shop: not driving there
    send(&mut k, V(0), P2, &hello_with(200, &HelloOpts { status: 4, ..seat2(b"Alex 2") }), None);
    assert_eq!(k.info("coopSeats"), 0.0);
    // the flag gone (another game on the port): no second player
    send(&mut k, V(0), P2, &hello_with(200, &seat1(b"Alex 2")), None);
    assert_eq!(k.info("coopSeats"), 0.0);
}

// a full hub (15360 endpoints: the free-roam share of the switch's 16000 places and co-op's second players), 2000
// coming as others go: the 15361st is refused, and every player keeps a slot under 16384, the most a parked world
// map's key names (a slot past it read as another one's, parked); the new ones' map shows each of them once
#[test]
fn a_full_hub_keeps_every_slot_a_maps_key_names() {
    let mut k = Kit::new(RoamOptions::default());
    let v = |i: u32| 0x0A4D_0100 + i;
    for i in 0..15360u32 {
        k.from(v(i), &hello(1000 + i));
    }
    k.from(v(20_000), &hello(99));
    assert_eq!((k.info("players"), k.info("refused")), (15360.0, 1.0));
    let late = fnv1a("late");
    for n in 0..2000u32 {
        k.from(v(n), &bye(1000 + n));
        let (vip, id) = (v(30_000 + n), 50_000 + n);
        k.from(vip, &hello_with(id, &HelloOpts { room: late, ..Default::default() }));
        k.from(vip, &snap(id, [(n % 50) as f64 * 20.0, (n / 50) as f64 * 20.0, 0.0]));
    }
    assert_eq!(k.info("players"), 15360.0);
    k.clear();
    // the world map of one of them, with who is parked and heights (the game's), as wide as it takes
    k.from(v(30_000), &mapreq_flags(50_000, 1, 4096, 0, 1 | 2));
    let keys: Vec<u16> = answers(&k).iter().flat_map(|(_, m, _)| m.5.iter().map(|e| e.0)).collect();
    assert_eq!(keys.len(), 1999, "the room's others");
    assert!(keys.iter().all(|&s| s < 0x4000), "a key past 16383: {:?}", keys.iter().filter(|&&s| s >= 0x4000).take(3).collect::<Vec<_>>());
    assert_eq!(keys.iter().collect::<HashSet<_>>().len(), 1999, "each once");
}
