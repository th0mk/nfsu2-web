//! How long a tick takes with a full hub: 2000 players in one room (the grid and the heap), in 20 rooms of 100 (every
//! pair of each), and in 8 rooms of 250 (every pair of rooms just under `exact_room`: the most pairs a tick sorts);
//! and the same with crews (bench_crews: crews of 2 to 6 spread over the map, and every player in 4 crews of 64, the
//! most a crew is linked first), and with blocks (bench_blocks: 1 player in 20 blocks 10 others).  Not a test: run it
//! in release,
//!   cargo.sh roam test --release -p nfsu2-roam --test bench -- --ignored --nocapture

mod kit;

use std::time::Instant;

use kit::*;
use nfsu2_roam::RoamOptions;
use nfsu2_util::cpu::process_cpu_seconds;

/// a small random walk of the bench's own
struct Lcg(u64);

impl Lcg {
    fn unit(&mut self) -> f64 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        (self.0 >> 11) as f64 / (1u64 << 53) as f64
    }
}

fn bench(what: &str, players: u32, rooms: u32, area: f64) {
    bench_moving(what, players, rooms, area, 15.0)
}

/// `step`: how far a player moves in x and y each second at most
fn bench_moving(what: &str, players: u32, rooms: u32, area: f64, step: f64) {
    bench_with(what, players, rooms, area, step, &|_| Vec::new(), &|_| Vec::new())
}

/// `crews(i)`: player i's crews (none: a HELLO of before, without the crew flag); players i of one room are the ones
/// with the same i % rooms; player i is named p<i>, and `blocks(i)` are the names it blocks
#[allow(clippy::type_complexity)]
fn bench_with(
    what: &str,
    players: u32,
    rooms: u32,
    area: f64,
    step: f64,
    crews: &dyn Fn(u32) -> Vec<u32>,
    blocks: &dyn Fn(u32) -> Vec<String>,
) {
    let mut k = Kit::new(RoamOptions { max_players: 4096, ..RoamOptions::default() });
    let mut r = Lcg(7);
    let mut pos: Vec<[f64; 3]> =
        (0..players).map(|_| [r.unit() * area - area / 2.0, r.unit() * area - area / 2.0, r.unit() * 20.0]).collect();
    let vip = |i: u32| 0x0A4D_0100 + i;
    let hellos: Vec<Vec<u8>> = (0..players)
        .map(|i| {
            let (c, room) = (crews(i), if rooms > 1 { 1 + i % rooms } else { 0 });
            let crews = (!c.is_empty()).then_some(&c[..]);
            let name = format!("p{i}");
            frame(vip(i), &hello_with(1000 + i, &HelloOpts { core: 2, room, crews, name: name.as_bytes(), ..Default::default() }))
        })
        .collect();
    for i in 0..players {
        let b = blocks(i);
        if !b.is_empty() {
            k.hub.blocks(vip(i), b);
        }
    }
    let (mut times, mut cpu) = (Vec::new(), Vec::new());
    let mut frames_ms = 0.0;
    for t in 0..23 {
        k.clear();
        let t0 = Instant::now();
        for i in 0..players {
            let p = &mut pos[i as usize];
            let v = [(r.unit() * 2.0 - 1.0) * step, (r.unit() * 2.0 - 1.0) * step];
            p[0] += v[0];
            p[1] += v[1];
            k.hub.frame(vip(i), &hellos[i as usize]);
            // its velocity: the move of this second
            let mut d = snap(1000 + i, *p);
            for a in 0..2 {
                d[38 + 4 * a..42 + 4 * a].copy_from_slice(&(v[a] as f32).to_le_bytes());
            }
            let n = d.len();
            let crc = crc32fast::hash(&d[..n - 4]);
            d[n - 4..].copy_from_slice(&crc.to_le_bytes());
            k.hub.frame(vip(i), &frame(vip(i), &d));
        }
        frames_ms += t0.elapsed().as_secs_f64() * 1000.0;
        let (t0, c0) = (Instant::now(), process_cpu_seconds());
        k.hub.tick();
        let ms = t0.elapsed().as_secs_f64() * 1000.0;
        // the first ticks make the links; from then on most are kept
        if t >= 3 {
            times.push(ms);
            cpu.push((process_cpu_seconds() - c0) * 1000.0);
        }
        k.step(1000.0);
    }
    times.sort_by(f64::total_cmp);
    cpu.sort_by(f64::total_cmp);
    let crewed = if k.info("crewLinks") > 0.0 || k.info("crewSkips") > 0.0 {
        format!(" ({} for crews, {} crews too large)", k.info("crewLinks"), k.info("crewSkips"))
    } else {
        String::new()
    };
    println!(
        "{what}: a tick {:.2} ms (median of {}; {:.2} to {:.2}; CPU {:.2} ms), {} links{crewed}; {:.0} ns a frame (HELLO or SNAP)",
        times[times.len() / 2],
        times.len(),
        times[0],
        times[times.len() - 1],
        cpu[cpu.len() / 2],
        k.info("links"),
        frames_ms * 1e6 / (23.0 * 2.0 * players as f64)
    );
}

/// crews of 2 to 6 players of one room, one after another (sizes 2, 3, 4, 5, 6 again and again: 500 crews of 2000
/// players), spread over the map as the players are
fn small_crews(rooms: u32) -> impl Fn(u32) -> Vec<u32> {
    move |i: u32| {
        let (room, q) = (i % rooms, i / rooms);
        // the cycle of 20 players: 5 crews
        let (cycle, at) = (q / 20, q % 20);
        let crew = [0, 0, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4][at as usize];
        vec![0x1000_0000 + room * 100_000 + cycle * 5 + crew]
    }
}

#[test]
#[ignore]
fn bench_2000_players_in_one_room() {
    bench("2000 players in one room of 3 km", 2000, 1, 3000.0);
    bench("2000 players in one room of 300 m", 2000, 1, 300.0);
    // where a game puts everyone at the start: a crowd on one spot, every other player within a cell or two
    bench_moving("2000 players on one spot of 10 m", 2000, 1, 10.0, 0.5);
}

#[test]
#[ignore]
fn bench_2000_players_in_20_rooms() {
    bench("2000 players in 20 rooms of 100", 2000, 20, 1000.0);
}

#[test]
#[ignore]
fn bench_2000_players_in_8_rooms_of_250() {
    bench("2000 players in 8 rooms of 250", 2000, 8, 1000.0);
}

/// each crowd of `bench_crews` without crews, then with them (the same players, places and moves): one after the
/// other, so that what else the machine does weighs on both alike
fn with_and_without(what: &str, players: u32, rooms: u32, area: f64, step: f64, crews: &dyn Fn(u32) -> Vec<u32>) {
    let at = what.find(',').unwrap_or(what.len());
    bench_with(&format!("{}, no crews", &what[..at]), players, rooms, area, step, &|_| Vec::new(), &|_| Vec::new());
    bench_with(what, players, rooms, area, step, crews, &|_| Vec::new());
}

#[test]
#[ignore]
fn bench_crews() {
    with_and_without("2000 players in one room of 3 km, 500 crews of 2-6", 2000, 1, 3000.0, 15.0, &small_crews(1));
    with_and_without("2000 players in 20 rooms of 100, crews of 2-6", 2000, 20, 1000.0, 15.0, &small_crews(20));
    with_and_without("2000 players in 8 rooms of 250, crews of 2-6", 2000, 8, 1000.0, 15.0, &small_crews(8));
    with_and_without("2000 players on one spot of 10 m, 500 crews of 2-6", 2000, 1, 10.0, 0.5, &small_crews(1));
    // the most a tick does for crews: every player in 4 crews of (at most) 64, each of the 4 a grouping of its own
    // (128 crews, 250,000 pairs)
    let big = |i: u32| (0..4).map(|k| 0x2000_0000 + k * 1000 + (i + 16 * k) / 64).collect();
    with_and_without("2000 players in one room of 3 km, each in 4 crews of 64", 2000, 1, 3000.0, 15.0, &big);
    with_and_without("4000 players in one room of 3 km, 1000 crews of 2-6", 4000, 1, 3000.0, 15.0, &small_crews(1));
}

#[test]
#[ignore]
fn bench_4000_players() {
    bench_moving("4000 players in one room of 3 km, 15 m/s", 4000, 1, 3000.0, 15.0);
    bench_moving("4000 players in one room of 3 km, 40 m/s", 4000, 1, 3000.0, 40.0);
    bench_moving("4000 players on one spot of 10 m", 4000, 1, 10.0, 0.5);
}

/// each crowd without blocks, then with 1 player in 20 blocking 10 others of its room (the same players, places and
/// moves)
#[test]
#[ignore]
fn bench_blocks() {
    let blocks = |rooms: u32| {
        move |i: u32| -> Vec<String> {
            if i % 20 != 0 {
                return Vec::new();
            }
            (1..=10).map(|k| format!("p{}", (i + k * 37 * rooms) % 2000)).collect()
        }
    };
    for (what, rooms, area, step) in [
        ("2000 players in one room of 3 km", 1, 3000.0, 15.0),
        ("2000 players in 8 rooms of 250", 8, 1000.0, 15.0),
        ("2000 players on one spot of 10 m", 1, 10.0, 0.5),
    ] {
        bench_with(&format!("{what}, no blocks"), 2000, rooms, area, step, &|_| Vec::new(), &|_| Vec::new());
        bench_with(&format!("{what}, 100 blocking 10 each"), 2000, rooms, area, step, &|_| Vec::new(), &blocks(rooms));
    }
}
