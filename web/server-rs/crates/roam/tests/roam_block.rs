//! Blocking (the page's hello "block", `Hub::blocks`): a browser that blocks a name is linked with no player of that
//! name, either way, and neither is on the other's world map; the others are as before; any case; until it leaves
//! the switch; the console says who blocks whom.  A blocked pair takes no place in either one's links, a crew links
//! no blocked pair, a name is its Online form, and a claimed name's keyphrase stays blocked under its next name.
//! (roam_lobby.rs: the race lobbies.)

mod kit;

use std::sync::Arc;

use kit::*;
use nfsu2_claims::{Claims, owner_of_token};
use nfsu2_roam::RoamOptions;

fn has(k: &Kit, i: u32, j: u32) -> bool {
    k.lists().get(&format!("{}:47210", V(i))).is_some_and(|l| l.iter().any(|e| e.vip == V(j)))
}

fn links(k: &Kit, i: u32) -> Vec<u32> {
    k.lists().get(&format!("{}:47210", V(i))).map_or(Vec::new(), |l| l.iter().map(|e| e.vip).collect())
}

/// player i of `name` at x (its HELLO and a SNAP)
fn place(k: &mut Kit, i: u32, name: &[u8], x: f64) {
    k.from(V(i), &hello_with(100 + i, &HelloOpts { name, core: 2, ..Default::default() }));
    k.from(V(i), &snap(100 + i, [x, 0.0, 0.0]));
}

#[test]
fn a_blocked_name_is_linked_with_nobody_who_blocks_it_either_way_until_they_leave() {
    let mut k = Kit::new(RoamOptions::default());
    let drive = |k: &mut Kit| {
        for (i, name) in [(0u32, &b"anna"[..]), (1, b"Bob"), (2, b"carl")] {
            k.from(V(i), &hello_with(100 + i, &HelloOpts { name, core: 2, ..Default::default() }));
            k.from(V(i), &snap(100 + i, [i as f64 * 10.0, 0.0, 0.0]));
        }
    };
    drive(&mut k);
    k.hub.blocks(V(0), vec!["bob".into()]);
    assert!(k.lines.lock().iter().any(|l| l == "[roam] 10.77.0.10 blocks \"bob\""), "{:?}", k.lines.lock());
    k.clear();
    k.hub.tick();
    assert!(!has(&k, 0, 1) && !has(&k, 1, 0), "anna blocks Bob: neither lists the other");
    assert!(has(&k, 0, 2) && has(&k, 2, 0) && has(&k, 1, 2) && has(&k, 2, 1), "carl still sees both");
    // the world map: Bob's (carl is linked: not on it) has no anna
    k.step(1000.0);
    drive(&mut k);
    k.clear();
    k.from(V(1), &mapreq(101, 1));
    let map: Vec<_> = k.sent.lock().iter().filter_map(|(_, f)| map_of(f)).flat_map(|m| m.4).collect();
    assert!(map.is_empty(), "no anna on Bob's map: {map:?}");
    // anna leaves the switch and comes back blocking nobody: linked again
    k.hub.peer_gone(V(0));
    drive(&mut k);
    k.clear();
    k.hub.tick();
    assert!(has(&k, 0, 1) && has(&k, 1, 0), "the block went with her");
}

// 2 links a player (every pair | the grid): anna's nearest is Bob, whom she blocks.  The pair takes no place: both
// still get 2 links, from the others (dropped after the links were chosen, anna had 1).  On the map before the first
// tick too.
#[test]
fn a_blocked_pair_takes_no_place_in_either_ones_links() {
    for (way, o) in ways() {
        let mut k = Kit::new(RoamOptions { degree: 2, min_degree: 2, ..o });
        for (i, name, x) in [(0u32, &b"anna"[..], 0.0), (1, b"Bob", 5.0), (2, b"carl", 30.0), (3, b"dave", 60.0), (4, b"eve", 90.0)] {
            place(&mut k, i, name, x);
        }
        k.hub.blocks(V(1), vec!["ANNA".into()]);
        k.from(V(0), &mapreq(100, 1));
        let map: Vec<_> = k.sent.lock().iter().filter_map(|(_, f)| map_of(f)).flat_map(|m| m.4).collect();
        assert!(!map.iter().any(|e| e.0 == 5), "{way}: Bob not on anna's map before a tick: {map:?}");
        k.clear();
        k.hub.tick();
        for (i, j) in [(0, 1), (1, 0)] {
            let l = links(&k, i);
            assert!(l.len() == 2 && !l.contains(&V(j)), "{way}: player {i}'s links {l:?}");
        }
    }
}

// anna, Bob and carl share a crew, 400 m apart (crew-mates are linked first, however far): anna blocks Bob
#[test]
fn a_crew_links_no_blocked_pair() {
    for (way, o) in ways() {
        let mut k = Kit::new(o);
        let crew = [fnv1a("night owls")];
        for (i, name) in [(0u32, &b"anna"[..]), (1, b"Bob"), (2, b"carl")] {
            k.from(V(i), &hello_with(100 + i, &HelloOpts { name, core: 7, crews: Some(&crew), ..Default::default() }));
            k.from(V(i), &snap(100 + i, [i as f64 * 400.0, 0.0, 0.0]));
        }
        k.hub.tick();
        assert!(has(&k, 0, 1), "{way}: crew-mates linked");
        k.hub.blocks(V(0), vec!["bob".into()]);
        k.clear();
        k.hub.tick();
        assert!(!has(&k, 0, 1) && !has(&k, 1, 0), "{way}: not the blocked pair");
        assert!(has(&k, 0, 2) && has(&k, 1, 2), "{way}: carl with both");
    }
}

// a name is its Online form (nfsu2-claims `canonical`): any case, a run of what is no letter or digit one _, cut to 15
#[test]
fn a_name_is_blocked_as_its_online_form() {
    let mut k = Kit::new(RoamOptions::default());
    place(&mut k, 0, b"anna", 0.0);
    place(&mut k, 1, b"Bob_The_Great", 10.0);
    place(&mut k, 2, b"carl", 20.0);
    place(&mut k, 3, b"Bobby", 30.0);
    k.hub.blocks(V(0), vec!["bob the  great!".into(), "  CARL ".into(), "?".into()]);
    k.hub.tick();
    assert_eq!(links(&k, 0), vec![V(3)], "Bob_The_Great and carl blocked, Bobby not");
}

// Bob's keyphrase claimed "Bob"; anna blocks "bob"; Bob changes its name to Robert: still blocked (the old name is
// that keyphrase's for 14 days); dave is not
#[test]
fn a_claimed_names_keyphrase_stays_blocked_under_its_next_name() {
    let owner = |c: char| -> Arc<str> { owner_of_token(&c.to_string().repeat(43)).unwrap() };
    let (bob, dave) = (owner('B'), owner('D'));
    let claims = Arc::new(Claims::in_memory());
    let now = 1_000_000;
    claims.claim("Bob", &bob, now).unwrap();
    let mut k = Kit::new(RoamOptions::default());
    k.hub.set_claims(Some(claims.clone()));
    let put = |k: &mut Kit, i: u32, name: &[u8], o: Option<&Arc<str>>| {
        let h = hello_with(100 + i, &HelloOpts { name, core: 2, ..Default::default() });
        k.hub.frame_from(V(i), &frame(V(i), &h), o);
        k.hub.frame_from(V(i), &frame(V(i), &snap(100 + i, [i as f64 * 10.0, 0.0, 0.0])), o);
    };
    put(&mut k, 0, b"anna", None);
    put(&mut k, 1, b"Bob", Some(&bob));
    put(&mut k, 2, b"dave", Some(&dave));
    k.hub.blocks(V(0), vec!["bob".into()]);
    k.hub.tick();
    assert!(!has(&k, 0, 1) && has(&k, 0, 2));
    claims.claim("Robert", &bob, now + 1000).unwrap();
    put(&mut k, 1, b"Robert", Some(&bob));
    k.clear();
    k.hub.tick();
    assert!(!has(&k, 0, 1) && !has(&k, 1, 0), "Robert is Bob's keyphrase");
    assert!(has(&k, 0, 2) && has(&k, 1, 2));
}
