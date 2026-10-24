//! Blocking (the page's hello "block", `Hub::blocks`): a browser that blocks a name is linked with no player of that
//! name, either way, and neither is on the other's world map; the others are as before; any case; until it leaves
//! the switch; the console says who blocks whom.

mod kit;

use kit::*;
use nfsu2_roam::RoamOptions;

fn has(k: &Kit, i: u32, j: u32) -> bool {
    k.lists().get(&format!("{}:47210", V(i))).is_some_and(|l| l.iter().any(|e| e.vip == V(j)))
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
