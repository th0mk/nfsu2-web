//! Free roam's hub and claimed names (nfsu2-claims): a player under a name another keyphrase claimed gets no links,
//! is on nobody's map and counts nowhere; its owner, and a name nobody claimed, play as before.

mod kit;

use std::sync::Arc;

use kit::*;
use nfsu2_claims::{Claims, owner_of_token};
use nfsu2_roam::RoamOptions;

const T0: i64 = 1_000_000;

fn owner(c: char) -> Arc<str> {
    owner_of_token(&c.to_string().repeat(43)).unwrap()
}

/// a player of `name` at x, whose page plays with `key`'s keyphrase (None: none)
fn place(k: &mut Kit, i: u32, name: &[u8], x: f64, key: Option<char>) {
    let o = key.map(owner);
    let h = hello_with(100 + i, &HelloOpts { core: 4, name, room: room_hash("claims"), ..Default::default() });
    k.hub.frame_from(V(i), &frame(V(i), &h), o.as_ref());
    k.hub.frame_from(V(i), &frame(V(i), &snap(100 + i, [x, 0.0, 0.0])), o.as_ref());
}

fn linked(k: &Kit, i: u32, j: u32) -> bool {
    k.lists().get(&format!("{}:47210", V(i))).is_some_and(|l| l.iter().any(|e| e.vip == V(j)))
}

#[test]
fn a_claimed_name_plays_alone_under_another_keyphrase() {
    let claims = Arc::new(Claims::in_memory());
    claims.claim("Alex", &owner('A'), T0).unwrap();
    let mut k = Kit::new(RoamOptions::default());
    k.hub.set_claims(Some(claims.clone()));
    place(&mut k, 0, b"Racer", 0.0, None);
    place(&mut k, 1, b"Alex", 10.0, None);
    place(&mut k, 2, b"alex", 20.0, Some('B'));
    place(&mut k, 3, b"Alex", 30.0, Some('A'));
    k.hub.tick();
    assert!(linked(&k, 0, 3) && linked(&k, 3, 0), "the owner plays with the others");
    for i in [1, 2] {
        assert!(!linked(&k, 0, i) && !linked(&k, i, 0) && !linked(&k, i, 3), "player {i} under another's name: no links");
    }
    assert_eq!(k.hub.in_rooms().get(&room_hash("claims")), Some(&2), "they count nowhere");
    assert_eq!(k.lines.lock().iter().filter(|l| l.contains("a name another keyphrase claimed")).count(), 2, "{:?}", k.lines.lock());
    // the claim let go: they play with everyone at their next HELLO
    claims.release(&owner('A'));
    k.clear();
    place(&mut k, 1, b"Alex", 10.0, None);
    place(&mut k, 2, b"alex", 20.0, Some('B'));
    k.hub.tick();
    assert!(linked(&k, 0, 1) && linked(&k, 0, 2));
    // a name claimed while its impostor drives: alone from its next HELLO on
    claims.claim("Racer", &owner('C'), T0).unwrap();
    k.clear();
    place(&mut k, 0, b"Racer", 0.0, None);
    k.hub.tick();
    assert!(!linked(&k, 0, 1) && !linked(&k, 1, 0));
}

#[test]
fn playing_holds_the_claim() {
    let claims = Arc::new(Claims::in_memory());
    claims.claim("Alex", &owner('A'), T0).unwrap();
    let mut k = Kit::new(RoamOptions::default());
    k.hub.set_claims(Some(claims.clone()));
    k.step(30.0 * 86_400_000.0);
    place(&mut k, 0, b"Alex", 0.0, Some('A'));
    let now = T0 + 30 * 86_400_000;
    assert_eq!(claims.mine(&owner('A'), now).unwrap().until, now + nfsu2_claims::HOLD_DAYS * nfsu2_claims::DAY_MS);
}
