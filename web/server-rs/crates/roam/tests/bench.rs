//! How long a tick takes with a full hub: 2000 players in one room (the grid and the heap), in 20 rooms of 100 (every
//! pair of each), and in 8 rooms of 250 (every pair of rooms just under `exact_room`: the most pairs a tick sorts);
//! and the same with crews (bench_crews: crews of 2 to 6 spread over the map, and every player in 4 crews of 64, the
//! most a crew is linked first), and with blocks (bench_blocks: 1 player in 20 blocks 10 others).  And what the
//! minimap's questions cost the hub's thread (bench_near), and a tick from 2000 to 16000 players (bench_scale).  A
//! tick's CPU is the process's and the hub's thread's own (the walks of a large room are read on more threads:
//! `RoamOptions::link_threads`, BENCH_THREADS=n to set it).  Not a test: run it in release,
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

/// the CPU time of the calling thread (ms; Linux: /proc/thread-self/schedstat, 0 elsewhere): a tick's own, without
/// the threads it reads its walks on (`RoamOptions::link_threads`)
fn thread_cpu_ms() -> f64 {
    std::fs::read_to_string("/proc/thread-self/schedstat")
        .ok()
        .and_then(|s| s.split_whitespace().next()?.parse::<f64>().ok())
        .map_or(0.0, |ns| ns / 1e6)
}

fn bench(what: &str, players: u32, rooms: u32, area: f64) {
    bench_moving(what, players, rooms, area, 15.0)
}

/// `step`: how far a player moves in x and y each second at most
fn bench_moving(what: &str, players: u32, rooms: u32, area: f64, step: f64) {
    bench_links(what, players, rooms, area, step, 0)
}

/// `links`: the links every player asks for (core version 10), 0: none said (a core before, 32)
fn bench_links(what: &str, players: u32, rooms: u32, area: f64, step: f64, links: u8) {
    bench_with(what, players, rooms, area, step, links, &|_| Vec::new(), &|_| Vec::new())
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
    links: u8,
    crews: &dyn Fn(u32) -> Vec<u32>,
    blocks: &dyn Fn(u32) -> Vec<String>,
) {
    let d = RoamOptions::default();
    let link_threads = std::env::var("BENCH_THREADS").ok().and_then(|v| v.parse().ok()).unwrap_or(d.link_threads);
    let mut k = Kit::new(RoamOptions { max_players: 16384, link_threads, ..d });
    let mut r = Lcg(7);
    let mut pos: Vec<[f64; 3]> =
        (0..players).map(|_| [r.unit() * area - area / 2.0, r.unit() * area - area / 2.0, r.unit() * 20.0]).collect();
    let vip = |i: u32| 0x0A4D_0100 + i;
    let hellos: Vec<Vec<u8>> = (0..players)
        .map(|i| {
            let (c, room) = (crews(i), if rooms > 1 { 1 + i % rooms } else { 0 });
            let crews = (!c.is_empty()).then_some(&c[..]);
            let name = format!("p{i}");
            let core = if links > 0 { 10 } else { 2 };
            frame(vip(i), &hello_with(1000 + i, &HelloOpts { core, room, crews, name: name.as_bytes(), links, ..Default::default() }))
        })
        .collect();
    for i in 0..players {
        let b = blocks(i);
        if !b.is_empty() {
            k.hub.blocks(vip(i), b);
        }
    }
    let (mut times, mut cpu, mut own) = (Vec::new(), Vec::new(), Vec::new());
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
        let (t0, c0, h0) = (Instant::now(), process_cpu_seconds(), thread_cpu_ms());
        k.hub.tick();
        let ms = t0.elapsed().as_secs_f64() * 1000.0;
        // the first ticks make the links; from then on most are kept
        if t >= 3 {
            times.push(ms);
            cpu.push((process_cpu_seconds() - c0) * 1000.0);
            own.push(thread_cpu_ms() - h0);
        }
        k.step(1000.0);
    }
    times.sort_by(f64::total_cmp);
    cpu.sort_by(f64::total_cmp);
    own.sort_by(f64::total_cmp);
    let crewed = if k.info("crewLinks") > 0.0 || k.info("crewSkips") > 0.0 {
        format!(" ({} for crews, {} crews too large)", k.info("crewLinks"), k.info("crewSkips"))
    } else {
        String::new()
    };
    println!(
        "{what}: a tick {:.2} ms (median of {}; {:.2} to {:.2}; CPU {:.2} ms, the hub's thread {:.2} ms, least {:.2}), {} links{crewed}; \
         {:.0} ns a frame (HELLO or SNAP)",
        times[times.len() / 2],
        times.len(),
        times[0],
        times[times.len() - 1],
        cpu[cpu.len() / 2],
        own[own.len() / 2],
        own[0],
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
    bench_with(&format!("{}, no crews", &what[..at]), players, rooms, area, step, 0, &|_| Vec::new(), &|_| Vec::new());
    bench_with(what, players, rooms, area, step, 0, crews, &|_| Vec::new());
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

/// the links a tick makes for players that ask for 32, 40 and 48 (core version 10): a crowd's links cost the tick
/// about their number
#[test]
#[ignore]
fn bench_links_32_40_48() {
    for links in [32u8, 40, 48] {
        bench_links(&format!("3000 players in one room of 4 km, 15 m/s, {links} links"), 3000, 1, 4000.0, 15.0, links);
        bench_links(&format!("3000 players in one room of 1 km, 15 m/s, {links} links"), 3000, 1, 1000.0, 15.0, links);
        bench_links(&format!("2000 players on one spot of 10 m, {links} links"), 2000, 1, 10.0, 0.5, links);
    }
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
        bench_with(&format!("{what}, no blocks"), 2000, rooms, area, step, 0, &|_| Vec::new(), &|_| Vec::new());
        bench_with(&format!("{what}, 100 blocking 10 each"), 2000, rooms, area, step, 0, &|_| Vec::new(), &blocks(rooms));
    }
}

/// a car of bench_near's: where it is, where it points, its speed and how fast it turns (each changed now and then)
struct Car {
    p: [f64; 2],
    h: f64,
    s: f64,
    w: f64,
}

/// The minimap's questions (NEARREQ, roam_game.c near_*): `players` in one room of `area` m square, `crowd` of them in
/// 300 m square in its middle (a car park everyone drives to); 30 % standing, the rest at 5 to 30 m/s (15 in the
/// crowd) turning now and then; each one's SNAP to the hub every `snap_ms`, its question every `ask_ms` for the 64
/// nearest within `radius` m.  What a question costs the hub's thread (its answer made and sent), an answer's bytes and what
/// it placed, and the thread's time a second for questions, SNAPs and ticks.
fn bench_near(what: &str, players: u32, crowd: u32, area: f64, snap_ms: f64, ask_ms: f64, radius: u16) {
    let mut k = Kit::new(RoamOptions { max_players: 16384, ..RoamOptions::default() });
    let mut r = Lcg(11);
    let vip = |i: u32| 0x0A4D_0100 + i;
    let mut cars: Vec<Car> = (0..players)
        .map(|i| {
            let side = if i < crowd { 300.0 } else { area };
            let fast = if i < crowd { 15.0 } else { 30.0 };
            let s = if r.unit() < 0.3 { 0.0 } else { 5.0 + r.unit() * (fast - 5.0) };
            Car { p: [r.unit() * side - side / 2.0, r.unit() * side - side / 2.0], h: r.unit() * std::f64::consts::TAU, s, w: 0.0 }
        })
        .collect();
    let hellos: Vec<Vec<u8>> =
        (0..players).map(|i| frame(vip(i), &hello_with(1000 + i, &HelloOpts { core: 9, ..Default::default() }))).collect();
    let (step, secs, warm) = (50.0, 16.0, 4.0);
    let (mut asked, mut answered, mut bytes, mut placed, mut gone, mut near) = (0u64, 0u64, 0u64, 0u64, 0u64, 0u64);
    let (mut ask_s, mut snap_s, mut tick_s, mut snaps, mut ticks) = (0.0, 0.0, 0.0, 0u64, 0u64);
    let mut held: Vec<u16> = vec![0; players as usize];
    let mut req: Vec<u16> = vec![0; players as usize];
    let n = (secs * 1000.0 / step) as u64;
    for t in 0..n {
        let ms = t as f64 * step;
        let counting = ms >= warm * 1000.0;
        for c in cars.iter_mut() {
            if r.unit() < step / 4000.0 {
                c.w = if r.unit() < 0.5 { 0.0 } else { (r.unit() * 2.0 - 1.0) * 0.5 };
            }
            c.h += c.w * step / 1000.0;
            c.p[0] += c.s * c.h.cos() * step / 1000.0;
            c.p[1] += c.s * c.h.sin() * step / 1000.0;
            if c.p[0].abs() > area / 2.0 || c.p[1].abs() > area / 2.0 {
                c.h += std::f64::consts::PI;
            }
        }
        // each player's HELLO every second, SNAP every snap_ms, question every ask_ms, at a phase of its own
        let t0 = Instant::now();
        for i in 0..players {
            let phase = (i as f64 * 7.0) % 1000.0;
            if ((ms + phase) % 1000.0) < step {
                k.hub.frame(vip(i), &hellos[i as usize]);
            }
            if ((ms + phase) % snap_ms) < step {
                let c = &cars[i as usize];
                let v = [c.s * c.h.cos(), c.s * c.h.sin(), 0.0];
                k.hub.frame(vip(i), &frame(vip(i), &snap_moving(1000 + i, [c.p[0], c.p[1], 0.0], v, c.h, 2)));
                snaps += counting as u64;
            }
        }
        if counting {
            snap_s += t0.elapsed().as_secs_f64();
        }
        if t % (500.0 / step) as u64 == 0 {
            let t0 = Instant::now();
            k.hub.tick();
            if counting {
                tick_s += t0.elapsed().as_secs_f64();
                ticks += 1;
            }
        }
        k.clear();
        let mut askers = Vec::new();
        let t0 = Instant::now();
        for i in 0..players {
            let phase = (i as f64 * 13.0) % ask_ms;
            if ((ms + phase) % ask_ms) < step {
                let q = &mut req[i as usize];
                *q = q.wrapping_add(1).max(1);
                k.hub.frame(vip(i), &frame(vip(i), &nearreq(1000 + i, *q, 64, held[i as usize], radius)));
                askers.push(i);
            }
        }
        if counting {
            ask_s += t0.elapsed().as_secs_f64();
            asked += askers.len() as u64;
        }
        for (to, f) in k.sent.lock().iter() {
            let Some(m) = neard_of(f) else { continue };
            let i = to - vip(0);
            held[i as usize] = m.0;
            if counting {
                answered += 1;
                bytes += (f.len() - 16) as u64;
                placed += m.5.iter().filter(|e| e.1.is_some()).count() as u64;
                gone += m.5.iter().filter(|e| e.1.is_none()).count() as u64;
                near += m.4 as u64;
            }
        }
        k.step(step);
    }
    let a = answered.max(1) as f64;
    let span = secs - warm;
    println!(
        "{what}: a question {:.0} ns, {answered} answers of {asked}: {:.0} B each ({:.1} placed, {:.1} gone, {:.0} near); \
         a SNAP {:.0} ns, a tick {:.2} ms; the hub's thread a second: questions {:.1} ms, SNAPs and HELLOs {:.1} ms, ticks {:.1} ms; \
         answers {:.0} kB/s",
        ask_s * 1e9 / asked.max(1) as f64,
        bytes as f64 / a,
        placed as f64 / a,
        gone as f64 / a,
        near as f64 / a,
        snap_s * 1e9 / snaps.max(1) as f64,
        tick_s * 1e3 / ticks.max(1) as f64,
        ask_s * 1e3 / span,
        snap_s * 1e3 / span,
        tick_s * 1e3 / span,
        bytes as f64 / span / 1000.0
    );
}

#[test]
#[ignore]
fn bench_near_minimap() {
    for (ask_ms, radius) in [(250.0, 660), (250.0, 350), (250.0, 200)] {
        let at = format!("questions {ask_ms} ms within {radius} m");
        bench_near(&format!("3000 in 4 km, 500 in 300 m; {at}"), 3000, 500, 4000.0, 500.0, ask_ms, radius);
        bench_near(&format!("12000 in 4 km, 1000 in 300 m; {at}"), 12000, 1000, 4000.0, 500.0, ask_ms, radius);
        bench_near(&format!("12000 in 8 km, 1000 in 300 m; {at}"), 12000, 1000, 8000.0, 500.0, ask_ms, radius);
    }
}

/// 2000 to 16000 players in one room, and 8000 and 16000 in a smaller one and in 8 rooms: what a tick costs past the
/// 12000 players of the switch (BENCH_THREADS=1: the walks on the hub's thread alone)
#[test]
#[ignore]
fn bench_scale() {
    for n in [2000u32, 4000, 6000, 8000, 12000, 16000] {
        bench_links(&format!("{n} players in one room of 8 km, 15 m/s, 32 links"), n, 1, 8000.0, 15.0, 32);
    }
    for n in [8000u32, 16000] {
        bench_links(&format!("{n} players in one room of 3 km, 15 m/s, 32 links"), n, 1, 3000.0, 15.0, 32);
        bench_links(&format!("{n} players in 8 rooms of 4 km, 15 m/s, 32 links"), n, 8, 4000.0, 15.0, 32);
    }
}
