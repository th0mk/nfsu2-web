//! Layers (`RoamOptions::layer_max`): a room's players are seated in layers of at most layer_max browsers as they
//! come, and only players of one layer see each other.
//! 1. the seats: layer 0 fills first, then 1, and so on; a seat left is taken before a new layer opens; a player
//!    keeps its layer; another endpoint of a browser sits with it; each room has layers of its own;
//! 2. what a layer keeps apart: the links, the world map (MAP and MAPD) and the race lobbies of the map;
//! 3. crews: a crew-mate sits in its crew's layer while it has room, else in the first with room.

mod kit;

use kit::*;
use nfsu2_roam::RoamOptions;

/// player i's address (10.77.1.0 and on), its id 1000 + i
#[allow(non_snake_case)]
fn U(i: u32) -> u32 {
    0x0A4D_0100 + i
}

/// player i in `room` with these crews, at (x, 0): its HELLO and its SNAP
fn put(k: &mut Kit, i: u32, room: u32, crews: Option<&[u32]>, x: f64) {
    k.from(U(i), &hello_with(1000 + i, &HelloOpts { core: 7, room, crews, ..Default::default() }));
    k.from(U(i), &snap(1000 + i, [x + 0.25, 0.25, 0.0]));
}

/// the players each player (by its number) is linked with, after a tick
fn links(k: &mut Kit, n: u32) -> Vec<Vec<u32>> {
    k.clear();
    k.hub.tick();
    let l = k.lists();
    (0..n)
        .map(|i| {
            let mut v: Vec<u32> = l.get(&format!("{}:47210", U(i))).map_or(vec![], |e| e.iter().map(|p| p.vip - U(0)).collect());
            v.sort_unstable();
            v
        })
        .collect()
}

/// the browsers of each layer of `room`, as the hub's info() has them (none: one layer only)
fn layers(k: &Kit, room: u32) -> Vec<u64> {
    let info = k.hub.info();
    let rooms = info["layers"].as_array().unwrap();
    rooms
        .iter()
        .find(|r| r["room"] == room)
        .map_or(vec![], |r| r["browsers"].as_array().unwrap().iter().map(|n| n.as_u64().unwrap()).collect())
}

fn small(layer_max: usize) -> RoamOptions {
    RoamOptions { layer_max, view_m: 1e-9, ..RoamOptions::default() }
}

#[test]
fn a_layer_holds_1000_browsers_by_default() {
    assert_eq!(RoamOptions::default().layer_max, 1000);
    let k = Kit::new(RoamOptions { layer_max: 0, ..RoamOptions::default() });
    assert_eq!(k.info("layerMax"), 1000.0, "a value that is not a positive number keeps its default");
}

// 7 players at one place, 3 a layer: layers of 3, 3 and 1, each linked within its layer only
#[test]
fn players_of_one_layer_see_each_other_only() {
    let mut k = Kit::new(small(3));
    for i in 0..7 {
        put(&mut k, i, 0, None, i as f64);
    }
    let l = links(&mut k, 7);
    assert_eq!(l, vec![vec![1, 2], vec![0, 2], vec![0, 1], vec![4, 5], vec![3, 5], vec![3, 4], vec![]]);
    assert_eq!(layers(&k, 0), vec![3, 3, 1]);
    // the 1000 of the default: all 7 in one layer, nothing listed
    let mut k = Kit::new(RoamOptions { view_m: 1e-9, ..RoamOptions::default() });
    for i in 0..7 {
        put(&mut k, i, 0, None, i as f64);
    }
    assert!(links(&mut k, 7).iter().all(|v| v.len() == 6));
    assert!(k.hub.info()["layers"].as_array().unwrap().is_empty());
}

// a seat left in layer 0 is the next player's, before layer 1's or a new one; the others stay where they are, and a
// player keeps its layer at its next HELLOs
#[test]
fn a_seat_left_is_taken_first_and_a_player_keeps_its_layer() {
    let mut k = Kit::new(small(2));
    for i in 0..4 {
        put(&mut k, i, 0, None, i as f64);
    }
    assert_eq!(links(&mut k, 4), vec![vec![1], vec![0], vec![3], vec![2]]);
    k.from(U(0), &bye(1000));
    put(&mut k, 4, 0, None, 4.0);
    // (and their HELLOs again: nobody moves)
    for i in 1..5 {
        put(&mut k, i, 0, None, i as f64);
    }
    assert_eq!(links(&mut k, 5), vec![vec![], vec![4], vec![3], vec![2], vec![1]]);
    assert_eq!(layers(&k, 0), vec![2, 2]);
}

// another endpoint of a browser sits with its first, even in a full layer, and counts once
#[test]
fn a_browsers_endpoints_sit_together() {
    let mut k = Kit::new(small(2));
    put(&mut k, 0, 0, None, 0.0);
    put(&mut k, 1, 0, None, 1.0);
    let other = |d: &[u8]| frame_ports(U(0), d, 47211, 47210);
    k.hub.frame(U(0), &other(&hello_with(2000, &HelloOpts { core: 7, ..Default::default() })));
    k.hub.frame(U(0), &other(&snap(2000, [2.25, 0.25, 0.0])));
    put(&mut k, 2, 0, None, 3.0);
    let l = links(&mut k, 3);
    assert_eq!(l[1], vec![0, 0], "player 1 sees both of browser 0's endpoints");
    assert!(l[2].is_empty(), "player 2 is in layer 1 alone");
    assert_eq!(layers(&k, 0), vec![2, 1]);
}

// each room has layers of its own; a player whose HELLO names another room is seated again there
#[test]
fn each_room_has_its_layers() {
    let blue = room_hash("blue");
    let mut k = Kit::new(small(2));
    for i in 0..3 {
        put(&mut k, i, 0, None, i as f64);
    }
    put(&mut k, 3, blue, None, 3.0);
    assert_eq!(links(&mut k, 4), vec![vec![1], vec![0], vec![], vec![]]);
    assert_eq!((layers(&k, 0), layers(&k, blue)), (vec![2, 1], Vec::<u64>::new()));
    put(&mut k, 2, blue, None, 2.0);
    assert_eq!(links(&mut k, 4), vec![vec![1], vec![0], vec![3], vec![2]], "player 2 went to blue's layer 0");
}

// the world map lists the players of its own layer only, as MAP and as MAPD, and so do its race lobbies
#[test]
fn the_world_map_shows_ones_layer() {
    // (one link a player: the map lists the rest of the layer)
    let mut k = Kit::new(RoamOptions { degree: 1, min_degree: 1, ..small(5) });
    for i in 0..8 {
        put(&mut k, i, 0, None, i as f64 * 10.0);
    }
    links(&mut k, 8);
    k.clear();
    k.from(U(0), &mapreq(1000, 1));
    let map = k.sent.lock().iter().find_map(|(_, f)| map_of(f)).unwrap();
    assert_eq!(map.3, 3, "MAP: 5 in layer 0, less itself and its link");
    k.clear();
    k.from(U(6), &mapreq_base(1006, 1, 2048, 0));
    let mapd = k.sent.lock().iter().find_map(|(_, f)| mapd_of(f)).unwrap();
    assert_eq!(mapd.4, 1, "MAPD: 3 in layer 1, less itself and its link");
}

// a crew-mate sits in the layer where most of its crew is while it has room, else in the first with room
#[test]
fn a_crew_plays_together_while_its_layer_has_room() {
    let crew = room_hash("owls");
    let mut k = Kit::new(small(3));
    put(&mut k, 0, 0, None, 0.0);
    put(&mut k, 1, 0, None, 1.0);
    put(&mut k, 2, 0, None, 2.0);
    put(&mut k, 3, 0, Some(&[crew]), 3.0);
    // layer 0 is full, layer 1 has its crew-mate: there, and not layer 0's seat that 0 leaves
    k.from(U(0), &bye(1000));
    put(&mut k, 4, 0, Some(&[crew]), 4.0);
    assert_eq!(links(&mut k, 5)[4], vec![3]);
    // layer 1 full with a third, and the crew's fourth goes to the first layer with room (0's seat in layer 0)
    put(&mut k, 5, 0, Some(&[crew]), 5.0);
    put(&mut k, 6, 0, Some(&[crew]), 6.0);
    let l = links(&mut k, 7);
    assert_eq!(l[5], vec![3, 4]);
    assert_eq!(l[6], vec![1, 2]);
}
