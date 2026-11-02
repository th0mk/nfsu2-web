//! Roads above roads: a gap in height that no road explains (a highway over a street) weighs more than it is
//! (RoamOptions level_*), so a player's links go to the cars on its own road first.  What weighs as before: two cars on
//! one road up a hill, a car over a bump, a player in a shop, and anyone at all while there is room.

mod kit;

use std::f64::consts::PI;

use kit::*;
use nfsu2_roam::RoamOptions;

/// browser i's address
#[allow(non_snake_case)]
fn W(i: u32) -> u32 {
    0x0A4D_0002 + i
}

/// a player of core version 10 asking for `links` links, in room 0
fn join(k: &mut Kit, i: u32, links: u8) {
    k.from(W(i), &hello_with(100 + i, &HelloOpts { core: 10, links, ..Default::default() }));
}

/// the links of a room: every pair weighed, or the grid's nearest; with view points (kit's `ways` has none), as the
/// server has them
fn both() -> [(&'static str, RoamOptions); 2] {
    let o = RoamOptions::default();
    [("every pair", o.clone()), ("the grid", RoamOptions { exact_room: 1, ..o })]
}

/// the rule turned off (an option that is not a positive number keeps its default: 1e-9 m a metre is none)
fn flat(o: &RoamOptions) -> RoamOptions {
    RoamOptions { level_k: 1e-9, ..o.clone() }
}

/// player i's links, sorted
fn links_of(k: &Kit, i: u32) -> Vec<u32> {
    let l = k.lists();
    let mut v: Vec<u32> = l[&format!("{}:47210", W(i))].iter().map(|e| e.vip - W(0)).collect();
    v.sort();
    v
}

/// one tick of a hub that has the players' HELLOs and SNAPs in free roam (place, velocity, heading); player 0's links
/// (it asks for `links`, the others for 48: their room is not what decides)
fn tick_with(o: RoamOptions, links: u8, snaps: &[([f64; 3], [f64; 3], f64)]) -> Vec<u32> {
    let mut k = Kit::new(o);
    for (i, &(p, v, h)) in snaps.iter().enumerate() {
        join(&mut k, i as u32, if i == 0 { links } else { 48 });
        k.from(W(i as u32), &snap_moving(100 + i as u32, p, v, h, 2));
    }
    k.clear();
    k.hub.tick();
    links_of(&k, 0)
}

// Player 0 drives along a highway 10 m over a street, at 30 m/s; four cars drive the street under it with it, four
// drive the highway 60 to 200 m off.  With 4 links it gets the four on the highway (as it always did the four under
// it), with 8 all eight: the ones under it wait for room, they are not left out.
#[test]
fn on_a_highway_the_cars_on_it_come_before_the_street_under_it() {
    let (up, run, back) = (10.0, [30.0, 0.0, 0.0], [-30.0, 0.0, 0.0]);
    let snaps = [
        ([0.0, 0.0, up], run, 0.0),
        ([5.0, 3.0, 0.0], run, 0.0),
        ([-8.0, -4.0, 0.0], run, 0.0),
        ([12.0, 2.0, 0.0], run, 0.0),
        ([-3.0, -2.0, 0.0], run, 0.0),
        ([60.0, 3.0, up], run, 0.0),
        ([85.0, -3.0, up], run, 0.0),
        ([110.0, 3.0, up], back, PI),
        ([200.0, -3.0, up], run, 0.0),
    ];
    for (how, o) in both() {
        assert_eq!(tick_with(o.clone(), 4, &snaps), [5, 6, 7, 8], "{how}: the highway's");
        assert_eq!(tick_with(flat(&o), 4, &snaps), [1, 2, 3, 4], "{how}, as before: the street's");
        assert_eq!(tick_with(o.clone(), 8, &snaps), [1, 2, 3, 4, 5, 6, 7, 8], "{how}: room for all");
    }
}

// Under a highway meet, the street's own: a street car's links go to the cars on the street 40 to 100 m off before the
// two parked straight over it; a highway car 50 m to the side (in sight) weighs as it is.
#[test]
fn under_a_highway_the_street_comes_first() {
    let snaps = [
        ([0.0, 0.0, 0.0], [0.0; 3], 0.0),
        ([3.0, 2.0, 12.0], [0.0; 3], 0.0),
        ([-6.0, -2.0, 12.0], [0.0; 3], PI),
        ([40.0, 0.0, 0.0], [0.0; 3], 0.0),
        ([0.0, -70.0, 0.0], [0.0; 3], 0.0),
        ([100.0, 10.0, 0.0], [0.0; 3], 0.0),
    ];
    for (how, o) in both() {
        assert_eq!(tick_with(o.clone(), 3, &snaps), [3, 4, 5], "{how}: the street's");
        assert_eq!(tick_with(flat(&o), 3, &snaps), [1, 2, 3], "{how}, as before");
    }
    let side = [([0.0, 0.0, 0.0], [0.0; 3], 0.0), ([0.0, 50.0, 12.0], [0.0; 3], 0.0), ([60.0, 0.0, 0.0], [0.0; 3], 0.0)];
    for (how, o) in both() {
        assert_eq!(tick_with(o, 1, &side), [1], "{how}: in sight");
    }
}

// Two cars on one road up a hill weigh as before (a road at 25 % for 60 m, at 20 % for 100 and 200 m), and so does a
// car 5 m up over a bump beside player 0, or 7 m up 10 m off: each comes before a car on flat ground a little farther
// off.  (Player 0 points along x: its view point is far from all of them.)
#[test]
fn up_a_hill_and_over_a_bump_as_before() {
    for (near, far) in [
        ([0.0, 60.0, 15.0], [0.0, -64.0, 0.0]),
        ([0.0, 100.0, 20.0], [0.0, -105.0, 0.0]),
        ([0.0, 200.0, 40.0], [0.0, -206.0, 0.0]),
        ([0.0, 3.0, 5.0], [0.0, -7.0, 0.0]),
        ([0.0, 10.0, 7.0], [0.0, -13.0, 0.0]),
    ] {
        let snaps = [([0.0, 0.0, 0.0], [0.0; 3], 0.0), (near, [0.0; 3], 0.0), (far, [0.0; 3], 0.0)];
        for (how, o) in both() {
            assert_eq!(tick_with(o.clone(), 1, &snaps), [1], "{how}: {near:?} before {far:?}");
            assert_eq!(tick_with(flat(&o), 1, &snaps), [1], "{how}, as before: {near:?} before {far:?}");
        }
    }
}

/// one tick of a hub that has the players' HELLOs and SNAPs in free roam (place, velocity, heading, the nose's slope);
/// player 0's links (it asks for `links`, the others for 48)
fn tick_posed(o: RoamOptions, links: u8, snaps: &[([f64; 3], [f64; 3], f64, f64)]) -> Vec<u32> {
    let mut k = Kit::new(o);
    for (i, &(p, v, h, slope)) in snaps.iter().enumerate() {
        join(&mut k, i as u32, if i == 0 { links } else { 48 });
        k.from(W(i as u32), &snap_pose(100 + i as u32, p, v, h, slope, 2));
    }
    k.clear();
    k.hub.tick();
    links_of(&k, 0)
}

// Up a hill, a car in front counts from the view point as it always did: the view point is up the slope the car's
// nose is on.  Player 0 parked facing up a road at 15 %, car 1 parked 150 m on up it (22.5 m higher), car 2 100 m
// beside it on flat ground: car 1 first.  (Were the view point at player 0's own height, 22.5 m under car 1, car 1
// would weigh as on a road above, and car 2 would come first.)
#[test]
fn up_a_hill_the_view_point_is_up_the_slope() {
    for (slope, want) in [(0.15, 1), (0.0, 2)] {
        let snaps = [
            ([0.0, 0.0, 0.0], [0.0; 3], 0.0, slope),
            ([150.0, 0.0, 22.5], [0.0; 3], 0.0, slope),
            ([0.0, -100.0, 0.0], [0.0; 3], -PI / 2.0, 0.0),
        ];
        for (how, o) in both() {
            assert_eq!(tick_posed(o.clone(), 1, &snaps), [want], "{how}: the nose {slope} up");
            assert_eq!(tick_posed(flat(&o), 1, &snaps), [1], "{how}, as before: the nose {slope} up");
        }
    }
}

// ...and so is the place 4 s ahead: player 0 drives up a road at 15 % at 25 m/s, car 1 parked 100 m on up it (15 m
// higher, facing down), car 2 parked 60 m beside: car 1 first, where player 0 will be in 4 s.
#[test]
fn up_a_hill_the_place_ahead_is_up_the_slope() {
    for (slope, want) in [(0.15, 1), (0.0, 2)] {
        let snaps = [
            ([0.0, 0.0, 0.0], [25.0, 0.0, 3.75], 0.0, slope),
            ([100.0, 0.0, 15.0], [0.0; 3], PI, -slope),
            ([0.0, 60.0, 0.0], [0.0; 3], PI / 2.0, 0.0),
        ];
        for (how, o) in both() {
            assert_eq!(tick_posed(o.clone(), 1, &snaps), [want], "{how}: the nose {slope} up");
            assert_eq!(tick_posed(flat(&o), 1, &snaps), [1], "{how}, as before: the nose {slope} up");
        }
    }
}

// A car going up fast (a hop, a ramp) is where its SNAP had it, not moved on at its speed up (a second at 8 m/s would
// put it 11 m up, over a road above): 3 m up 5 m off, it comes before a car 20 m off.
#[test]
fn a_car_going_up_is_where_it_is() {
    let snaps = [([0.0, 0.0, 0.0], [0.0; 3], 0.0), ([0.0, 5.0, 3.0], [0.0, 2.0, 8.0], 0.0), ([0.0, -20.0, 0.0], [0.0; 3], 0.0)];
    for (how, o) in both() {
        assert_eq!(tick_with(o, 1, &snaps), [1], "{how}");
    }
}

// a player in a shop still counts 100 m up, at least, under the highway or on it: a car in free roam 80 m off comes
// before it at 10 m
#[test]
fn a_player_in_a_shop_counts_100_m_up() {
    for z in [0.0, 12.0] {
        for (how, o) in both() {
            let mut k = Kit::new(RoamOptions { degree: 1, ..o });
            for i in 0..3 {
                join(&mut k, i, 1);
            }
            k.from(W(0), &snap(100, [0.0, 0.0, 0.0]));
            k.from(W(1), &snap_n(101, [10.0, 0.0, z], 4, 1));
            k.from(W(2), &snap(102, [80.0, 0.0, 0.0]));
            k.hub.tick();
            assert_eq!(links_of(&k, 0), [2], "{how}: the shop's player at {z} m");
        }
    }
}

/// a number of [0, 1) from i and t (the same every run)
fn hash(i: u32, t: u32) -> f64 {
    let x = (u64::from(i) << 32 | u64::from(t)).wrapping_mul(0x9E37_79B9_7F4A_7C15);
    ((x ^ x >> 29).wrapping_mul(0xBF58_476D_1CE4_E5B9) >> 11) as f64 / (1u64 << 53) as f64
}

// The grid's three walks give the links every pair sorted gives, tick after tick, on roads above roads: a highway 12 m
// over a street, a crowd on each (one over the other), a hill at 20 %, cars hopping over bumps, players in a shop or
// paused, 16, 32 or 48 links each; and read on four threads, the same lists, byte for byte
#[test]
fn the_grid_links_as_every_pair_on_roads_above_roads() {
    let n = 500u32;
    let ground = |x: f64| if x > 300.0 { (x - 300.0) * 0.2 } else { 0.0 };
    let start: Vec<([f64; 3], [f64; 3])> = (0..n)
        .map(|i| {
            let (x, y, a, b) = ((hash(i, 0) - 0.5) * 1600.0, (hash(i, 1) - 0.5) * 1600.0, hash(i, 2) - 0.5, hash(i, 3) - 0.5);
            match i % 5 {
                // the two crowds, one over the other
                0 => ([a * 60.0, b * 16.0, if i % 2 == 0 { 12.0 } else { 0.0 }], [0.0; 3]),
                // on the highway (y near 0), along x
                1 => ([x, b * 16.0, 12.0], [if a < 0.0 { 40.0 } else { -40.0 }, 0.0, 0.0]),
                // on the streets, the hill's too
                _ => ([x, y, ground(x)], [a * 50.0, b * 50.0, 0.0]),
            }
        })
        .collect();
    let run = |o: RoamOptions| {
        let mut k = Kit::new(o);
        for i in 0..n {
            join(&mut k, i, [16u8, 32, 48][(i % 3) as usize]);
        }
        let mut cars = start.clone();
        let mut sent = Vec::new();
        for t in 0..10u32 {
            for (i, (p, v)) in cars.iter_mut().enumerate() {
                let i = i as u32;
                if i % 5 >= 1 {
                    p[0] = (p[0] + v[0] * 0.5).clamp(-800.0, 800.0);
                    p[1] = (p[1] + v[1] * 0.5).clamp(-800.0, 800.0);
                }
                if i % 5 >= 2 {
                    // a hop over a bump now and then, up to 9 m
                    p[2] = ground(p[0]) + if hash(i, 10 + t) < 0.1 { hash(i, 100 + t) * 9.0 } else { 0.0 };
                }
                let status = if i.is_multiple_of(13) {
                    4
                } else if i.is_multiple_of(17) {
                    5
                } else {
                    2
                };
                k.from(W(i), &snap_moving(100 + i, *p, *v, v[1].atan2(v[0]), status));
            }
            k.clear();
            k.hub.tick();
            sent.push((k.lists(), k.sent.lock().clone()));
            k.step(500.0);
        }
        sent
    };
    let every = run(RoamOptions { exact_room: 1000, ..RoamOptions::default() });
    let grid = run(RoamOptions { exact_room: 1, link_threads: 1, ..RoamOptions::default() });
    let grid4 = run(RoamOptions { exact_room: 1, link_threads: 4, ..RoamOptions::default() });
    for (t, ((a, _), (b, sb))) in every.iter().zip(&grid).enumerate() {
        assert_eq!(a.len(), n as usize);
        for (key, l) in a {
            let mut x: Vec<u32> = l.iter().map(|e| e.vip).collect();
            let mut y: Vec<u32> = b[key].iter().map(|e| e.vip).collect();
            x.sort();
            y.sort();
            assert_eq!(x, y, "tick {t}, player {key}");
        }
        assert!(*sb == grid4[t].1, "tick {t}: on four threads, the same lists");
    }
}
