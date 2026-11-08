//! A car meet's links (hub.rs `one_way`, `Gather::key`): a player slower than near_fast gets the cars within near_m
//! first, by where they are now, and one that came near stays near_hold_ms while it is; and the hints of a core of
//! version 16 (`lists`): which of the players who only get a player's car are far off, and why the cars near it went.

mod kit;

use kit::*;
use nfsu2_roam::RoamOptions;

#[allow(non_snake_case)]
fn W(i: u32) -> u32 {
    0x0A4D_0002 + i
}

const TX: u8 = 0x40;

/// player i's last list: the vips it gets cars from (entries not marked TX)
fn gets(k: &Kit, i: u32) -> Vec<u32> {
    let sent = k.sent.lock();
    let Some((_, f)) = sent.iter().rev().find(|(v, _)| *v == W(i)) else { return Vec::new() };
    let e = peers_of(f);
    let marks = crew_marks(f).map(|m| m.1).unwrap_or_else(|| vec![0; e.len()]);
    e.iter().zip(marks).filter(|(_, m)| m & TX == 0).map(|(p, _)| p.vip).collect()
}

/// the hints at the end of player i's last list (after its marks): the far ones (entry index, its byte) and the cars
/// gone (id, why); None without
fn hints(k: &Kit, i: u32) -> Option<(Vec<(u32, u8)>, Vec<(u32, u8)>)> {
    let sent = k.sent.lock();
    let (_, f) = sent.iter().rev().find(|(v, _)| *v == W(i))?;
    let e = peers_of(f);
    let d = &f[16..];
    if u16::from_le_bytes([d[14], d[15]]) & HF_CREW == 0 {
        return None;
    }
    let n = e.len();
    let o = 17 + 10 * n;
    let mut o = o + 1 + 4 * d[o] as usize + n;
    let end = d.len() - 4;
    if o >= end {
        return None;
    }
    let nf = d[o] as usize;
    let far = (0..nf).map(|k| (e[(d[o + 1 + k] & 0x7F) as usize].vip, d[o + 1 + k])).collect();
    o += 1 + nf;
    let ng = d[o] as usize;
    let gone = (0..ng).map(|k| (u32::from_le_bytes(d[o + 1 + 5 * k..o + 5 + 5 * k].try_into().unwrap()), d[o + 5 + 5 * k])).collect();
    Some((far, gone))
}

fn join(k: &mut Kit, i: u32, core: u16, links: u8, p: [f64; 3], v: [f64; 3]) {
    k.from(W(i), &hello_with(100 + i, &HelloOpts { core, links, ..Default::default() }));
    k.from(W(i), &snap_moving(100 + i, p, v, 0.0, 2));
}

/// the players say where they are again (as every half second), then a tick
fn tick(k: &mut Kit, at: &[([f64; 3], [f64; 3])], ms: f64) {
    k.step(ms);
    for (i, (p, v)) in at.iter().enumerate() {
        k.from(W(i as u32), &snap_moving(100 + i as u32, *p, *v, 0.0, 2));
    }
    k.clear();
    k.hub.tick();
}

/// A player parked at a meet who shows 2 cars, two parked 30 and 40 m off: a third that parks 10 m from it shows only
/// once the two have shown 30 s (near_hold_ms), then in place of the farther one; before (near_m none), after 5 s
/// (hold_ms)
#[test]
fn a_car_near_a_parked_player_stays_30_s() {
    for near in [true, false] {
        let o = RoamOptions { view_m: 1e-9, near_m: if near { 60.0 } else { 1e-9 }, ..RoamOptions::default() };
        let mut k = Kit::new(o);
        let mut at =
            vec![([0.0, 0.0, 0.0], [0.0; 3]), ([30.0, 0.0, 0.0], [0.0; 3]), ([0.0, 40.0, 0.0], [0.0; 3]), ([400.0, 0.0, 0.0], [0.0; 3])];
        for (i, (p, v)) in at.iter().enumerate() {
            join(&mut k, i as u32, 16, if i == 0 { 2 } else { 8 }, *p, *v);
        }
        k.hub.tick();
        assert_eq!(gets(&k, 0), vec![W(1), W(2)], "near {near}: the two near ones");
        // the third drives in and parks 10 m off
        at[3].0 = [-10.0, 0.0, 0.0];
        let mut swapped = None;
        for t in 1..=80 {
            tick(&mut k, &at, 500.0);
            let g = gets(&k, 0);
            if g.contains(&W(3)) && swapped.is_none() {
                swapped = Some(t as f64 / 2.0);
                assert!(g.contains(&W(1)) && !g.contains(&W(2)), "near {near}: the farther one gives way ({g:x?})");
            }
        }
        let s = swapped.expect("the nearest shows in the end");
        if near {
            assert!((29.0..=31.0).contains(&s), "near first: the swap after 30 s, not {s} s");
        } else {
            assert!(s <= 6.0, "before: the swap after hold_ms, not {s} s");
        }
    }
}

/// ...and the hub tells the player why the car went (its hints: RC_GONE_ROOM), and counts it
#[test]
fn a_near_car_that_goes_is_said_why() {
    let o = RoamOptions { view_m: 1e-9, near_hold_ms: 1000.0, hold_ms: 1000.0, ..RoamOptions::default() };
    let mut k = Kit::new(o);
    let mut at =
        vec![([0.0, 0.0, 0.0], [0.0; 3]), ([30.0, 0.0, 0.0], [0.0; 3]), ([0.0, 40.0, 0.0], [0.0; 3]), ([400.0, 0.0, 0.0], [0.0; 3])];
    for (i, (p, v)) in at.iter().enumerate() {
        join(&mut k, i as u32, 16, if i == 0 { 2 } else { 8 }, *p, *v);
    }
    k.hub.tick();
    at[3].0 = [-10.0, 0.0, 0.0];
    let mut said = None;
    for _ in 0..10 {
        tick(&mut k, &at, 500.0);
        if let Some((_, gone)) = hints(&k, 0)
            && !gone.is_empty()
        {
            said = Some(gone);
            break;
        }
    }
    assert_eq!(said, Some(vec![(102, 1)]), "player 2 (id 102) went: our links were full of nearer ones (RC_GONE_ROOM)");
    assert_eq!(k.hub.info()["nearGone"][0].as_u64(), Some(1), "counted: {}", k.hub.info()["nearGone"]);
    // the line every 5 minutes
    for _ in 0..610 {
        tick(&mut k, &at, 500.0);
    }
    {
        let lines = k.lines.lock();
        assert!(
            lines.iter().any(
                |l| l == "[roam] cars within 60 m of a player that went in the last 5 min: 1 (0.2 a minute): the player's links full 1"
            ),
            "{lines:?}"
        );
    }
    // a core of version 15 gets no hints
    let mut k = Kit::new(RoamOptions { view_m: 1e-9, near_hold_ms: 1000.0, hold_ms: 1000.0, ..RoamOptions::default() });
    for (i, (p, v)) in at.iter().enumerate() {
        join(&mut k, i as u32, 15, if i == 0 { 2 } else { 8 }, *p, *v);
    }
    for _ in 0..6 {
        tick(&mut k, &at, 500.0);
        assert!(hints(&k, 0).is_none(), "no hints to a core of 15");
    }
}

/// A driver at speed (near_fast and more) gets its cars as before: the near ones count as any other
#[test]
fn a_driver_at_speed_gets_its_cars_as_before() {
    let lists = |near: f64| {
        let o = RoamOptions { view_m: 1e-9, near_m: near, ..RoamOptions::default() };
        let mut k = Kit::new(o);
        // the driver at 25 m/s along x, cars round it 20 to 200 m off, some parked, some driving
        let mut at: Vec<([f64; 3], [f64; 3])> = vec![([0.0, 0.0, 0.0], [25.0, 0.0, 0.0])];
        for i in 1..40 {
            let a = i as f64 * 2.4;
            let r = 20.0 + 5.0 * i as f64;
            at.push(([r * a.cos(), r * a.sin(), 0.0], if i % 3 == 0 { [0.0, 12.0, 0.0] } else { [0.0; 3] }));
        }
        for (i, (p, v)) in at.iter().enumerate() {
            join(&mut k, i as u32, 16, if i == 0 { 8 } else { 32 }, *p, *v);
        }
        let mut out = Vec::new();
        for _ in 0..40 {
            for (p, v) in at.iter_mut() {
                p[0] += v[0] * 0.5;
                p[1] += v[1] * 0.5;
            }
            tick(&mut k, &at, 500.0);
            let mut g = gets(&k, 0);
            g.sort();
            out.push(g);
        }
        out
    };
    assert_eq!(lists(60.0), lists(1e-9), "the driver's cars, near first or not");
}

/// A player who only gets our car, far off: our list says so (every 4th past 300 m, every 10th past 1 km), so our core
/// sends it that few; a near one is not named; a core of version 15 gets none of it
#[test]
fn the_ones_far_off_who_only_get_our_car_are_hinted() {
    for core in [16u16, 15] {
        let mut k = Kit::new(RoamOptions { view_m: 1e-9, ..RoamOptions::default() });
        // player 0 shows 1 car (the nearest, 3 at 200 m); 1 (500 m) and 2 (1500 m) show 64 and get its car
        let at =
            [([0.0, 0.0, 0.0], [0.0; 3]), ([500.0, 0.0, 0.0], [0.0; 3]), ([0.0, 1500.0, 0.0], [0.0; 3]), ([200.0, 0.0, 0.0], [0.0; 3])];
        for (i, (p, v)) in at.iter().enumerate() {
            join(&mut k, i as u32, core, if i == 0 { 1 } else { 64 }, *p, *v);
        }
        k.hub.tick();
        tick(&mut k, &at, 500.0);
        assert_eq!(gets(&k, 0), vec![W(3)], "core {core}: it gets the nearest one's car only");
        let h = hints(&k, 0);
        if core == 15 {
            assert!(h.is_none(), "no hints to a core of 15: {h:?}");
            continue;
        }
        let (far, gone) = h.expect("hints");
        let mut far: Vec<(u32, bool)> = far.into_iter().map(|(v, b)| (v, b & 0x80 != 0)).collect();
        far.sort();
        assert_eq!(far, vec![(W(1), false), (W(2), true)], "far (500 m) and very far (1500 m); the one at 200 m gets every one");
        assert!(gone.is_empty());
        assert_eq!(k.hub.info()["txOnly"], serde_json::json!([0, 1, 1]), "{}", k.hub.info()["txOnly"]);
        // two that drive at each other count as near before they are (4 s on at their speeds)
        let at2 = [([0.0, 0.0, 0.0], [30.0, 0.0, 0.0]), ([500.0, 0.0, 0.0], [-30.0, 0.0, 0.0]), at[2], at[3]];
        tick(&mut k, &at2, 500.0);
        let (far, _) = hints(&k, 0).expect("hints");
        assert!(far.iter().all(|e| e.0 != W(1)), "closing in: near ({far:?})");
    }
}
