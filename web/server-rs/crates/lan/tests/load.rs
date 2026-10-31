//! The switch under free roam's load (ignored by default; in release, with the output):
//!   cargo test -p nfsu2-lan --release --test load -- --ignored --nocapture
//! N players in free roam on pages that take bundles, each sending 20 bundles a second, a snapshot of 94 bytes to its
//! 32 friends in each (free roam's most common case), for N = 120, 480 and 1000.  It reports the datagrams delivered a
//! second (N x 640 when nothing is lost), the messages they came in, and the CPU: the switch's threads (their own
//! runtime, "lan-switch"), the fake players' ("lan-players"), the whole process.  The target (lan.rs's crate doc): 1500
//! players in free roam, ~1,000,000 deliveries a second, on all of a 16-core box.

mod harness;

use std::sync::Arc;
use std::time::{Duration, Instant};

use harness::*;
use nfsu2_lan::LanOptions;
use nfsu2_util::Counter;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

const FRIENDS: usize = 32;
const HZ: u64 = 20;
const SNAP: usize = 94;

/// CPU seconds of the process's threads whose names start with `prefix` (/proc/self/task/*/stat)
fn thread_cpu(prefix: &str) -> f64 {
    let (u, s) = thread_cpu_split(prefix);
    u + s
}

/// the same in user space and in the kernel (where the switch's sockets cost what they cost)
fn thread_cpu_split(prefix: &str) -> (f64, f64) {
    let tick = 100.0; // USER_HZ
    let (mut u, mut s) = (0.0, 0.0);
    let Ok(dir) = std::fs::read_dir("/proc/self/task") else { return (0.0, 0.0) };
    for t in dir.flatten() {
        let Ok(stat) = std::fs::read_to_string(t.path().join("stat")) else { continue };
        let (Some(a), Some(b)) = (stat.find('('), stat.rfind(')')) else { continue };
        if !stat[a + 1..b].starts_with(prefix) {
            continue;
        }
        let f: Vec<&str> = stat[b + 2..].split(' ').collect();
        // after the name: state is field 3; utime 14 and stime 15 (1-based over the whole line)
        let ut: f64 = f.get(11).and_then(|x| x.parse().ok()).unwrap_or(0.0);
        let st: f64 = f.get(12).and_then(|x| x.parse().ok()).unwrap_or(0.0);
        u += ut / tick;
        s += st / tick;
    }
    (u, s)
}

fn raise_nofile() {
    // SAFETY: getrlimit/setrlimit only read and write the struct they are given
    unsafe {
        let mut r: libc::rlimit = std::mem::zeroed();
        if libc::getrlimit(libc::RLIMIT_NOFILE, &mut r) == 0 {
            r.rlim_cur = r.rlim_max;
            libc::setrlimit(libc::RLIMIT_NOFILE, &r);
        }
    }
}

/// a client's WebSocket frame (masked with a zero key: the payload as it is)
fn client_frame(op: u8, payload: &[u8]) -> Vec<u8> {
    let mut f = vec![0x80 | op];
    if payload.len() < 126 {
        f.push(0x80 | payload.len() as u8);
    } else {
        f.push(0x80 | 126);
        f.extend_from_slice(&(payload.len() as u16).to_be_bytes());
    }
    f.extend_from_slice(&[0, 0, 0, 0]);
    f.extend_from_slice(payload);
    f
}

/// the server's frames in `buf` (unmasked): each complete one to `f(opcode, payload)`, the rest kept
fn frames(buf: &mut Vec<u8>, mut f: impl FnMut(u8, &[u8])) {
    let mut at = 0;
    loop {
        if buf.len() < at + 2 {
            break;
        }
        let op = buf[at] & 15;
        let (len, h) = match buf[at + 1] & 127 {
            126 if buf.len() >= at + 4 => (u16::from_be_bytes([buf[at + 2], buf[at + 3]]) as usize, 4),
            127 if buf.len() >= at + 10 => (u64::from_be_bytes(buf[at + 2..at + 10].try_into().unwrap()) as usize, 10),
            126 | 127 => break,
            n => (n as usize, 2),
        };
        if buf.len() < at + h + len {
            break;
        }
        f(op, &buf[at + h..at + h + len]);
        at += h + len;
    }
    buf.drain(..at);
}

/// a player on the switch: its socket, its address
async fn join(port: u16, i: usize) -> (TcpStream, u32) {
    let mut s = raw_ws(port, &format!("?g=p{i}")).await;
    let mut buf = Vec::new();
    let mut tmp = vec![0u8; 16 << 10];
    let mut vip = 0;
    while vip == 0 {
        let n = s.read(&mut tmp).await.unwrap();
        assert!(n > 0, "the welcome");
        buf.extend_from_slice(&tmp[..n]);
        frames(&mut buf, |op, p| {
            if op == 1 {
                let m: serde_json::Value = serde_json::from_slice(p).unwrap();
                if m["t"] == "welcome" {
                    vip = m["vip"].as_u64().unwrap() as u32;
                }
            }
        });
    }
    let hello = format!(r#"{{"t":"hello","name":"P{i}","lan":0,"bundle":1}}"#);
    s.write_all(&client_frame(1, hello.as_bytes())).await.unwrap();
    (s, vip)
}

struct Counts {
    delivered: Counter,
    messages: Counter,
    sent: Counter,
}

async fn crowd(port: u16, n: usize, warm: Duration, run: Duration, sw: Arc<nfsu2_lan::LanSwitch>) {
    let mut players = Vec::with_capacity(n);
    for i in 0..n {
        players.push(join(port, i).await);
    }
    let vips: Vec<u32> = players.iter().map(|p| p.1).collect();
    let counts = Arc::new(Counts { delivered: Counter::new(), messages: Counter::new(), sent: Counter::new() });
    let stop = tokio_util::sync::CancellationToken::new();
    let t0 = tokio::time::Instant::now() + Duration::from_millis(200);
    let mut tasks = Vec::new();
    for (i, (s, _)) in players.into_iter().enumerate() {
        let (mut r, mut w) = s.into_split();
        // a snapshot to the next 32 players round the ring, in one bundle entry (n = 32)
        let friends: Vec<u32> = (1..=FRIENDS.min(n - 1)).map(|k| vips[(i + k) % n]).collect();
        let snap = frame_of(0, &[i as u8; SNAP - 16], 1, 47210, 47210);
        let bundle = client_frame(2, &bundle_of(&[(snap, friends)]));
        let c = counts.clone();
        let st = stop.clone();
        // each player's ticks at a phase of its own, at random (in ring order a player's friends would send within a
        // few ms of each other, and their snapshots reach it as one burst: an easy case for the bundles)
        let phase = Duration::from_micros(nfsu2_util::rand::u64() % (1_000_000 / HZ));
        tasks.push(tokio::spawn(async move {
            let mut iv = tokio::time::interval_at(t0 + phase, Duration::from_millis(1000 / HZ));
            iv.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
            loop {
                tokio::select! {
                    _ = st.cancelled() => return,
                    _ = iv.tick() => {}
                }
                if w.write_all(&bundle).await.is_err() {
                    return;
                }
                c.sent.inc();
            }
        }));
        let c = counts.clone();
        let st = stop.clone();
        tasks.push(tokio::spawn(async move {
            let mut buf = Vec::with_capacity(64 << 10);
            let mut tmp = vec![0u8; 64 << 10];
            loop {
                let n = tokio::select! {
                    _ = st.cancelled() => return,
                    n = r.read(&mut tmp) => match n { Ok(0) | Err(_) => return, Ok(n) => n },
                };
                buf.extend_from_slice(&tmp[..n]);
                let (mut d, mut m) = (0, 0);
                frames(&mut buf, |op, p| {
                    if op != 2 || p.is_empty() {
                        return;
                    }
                    m += 1;
                    if p[0] != 32 {
                        d += 1;
                        return;
                    }
                    let mut at = 4;
                    while at + 4 <= p.len() {
                        let f = at + 4 + 4 * p[at] as usize;
                        at = f + u16::from_be_bytes([p[at + 2], p[at + 3]]) as usize;
                        d += 1;
                    }
                });
                c.delivered.add(d);
                c.messages.add(m);
            }
        }));
    }
    // the switch's ping as a page measures it ({"t":"ping"} and its pong), every 100 ms (under the text rate) from a
    // player of its own, while the crowd's numbers are taken
    let (probe, _) = join(port, n).await;
    let lat = Arc::new(parking_lot::Mutex::new(Vec::new()));
    let (l2, st) = (lat.clone(), stop.clone());
    let pinger = tokio::spawn(async move {
        let (mut r, mut w) = probe.into_split();
        let mut buf = Vec::new();
        let mut tmp = vec![0u8; 64 << 10];
        tokio::time::sleep_until(t0 + warm).await;
        for id in 0u64.. {
            let t = Instant::now();
            if w.write_all(&client_frame(1, format!(r#"{{"t":"ping","id":{id}}}"#).as_bytes())).await.is_err() {
                return;
            }
            let mut back = false;
            while !back {
                let n = tokio::select! {
                    _ = st.cancelled() => return,
                    n = r.read(&mut tmp) => match n { Ok(0) | Err(_) => return, Ok(n) => n },
                };
                buf.extend_from_slice(&tmp[..n]);
                frames(&mut buf, |op, p| {
                    if op == 1 && serde_json::from_slice::<serde_json::Value>(p).is_ok_and(|m| m["t"] == "pong" && m["id"] == id) {
                        back = true;
                    }
                });
            }
            l2.lock().push(t.elapsed().as_secs_f64() * 1000.0);
            tokio::select! {
                _ = st.cancelled() => return,
                _ = tokio::time::sleep(Duration::from_millis(100)) => {}
            }
        }
    });
    tasks.push(pinger);
    tokio::time::sleep_until(t0 + warm).await;
    let (d0, m0, s0) = (counts.delivered.get(), counts.messages.get(), counts.sent.get());
    let f0 = sw.info().await["frames"].as_u64().unwrap();
    let (cs0, cp0, all0) = (thread_cpu_split("lan-switch"), thread_cpu("lan-players"), nfsu2_util::cpu::process_cpu_seconds());
    let at = Instant::now();
    tokio::time::sleep(run).await;
    let secs = at.elapsed().as_secs_f64();
    let (cs1, cp1, all1) = (thread_cpu_split("lan-switch"), thread_cpu("lan-players"), nfsu2_util::cpu::process_cpu_seconds());
    let (d1, m1, s1) = (counts.delivered.get(), counts.messages.get(), counts.sent.get());
    let f1 = sw.info().await["frames"].as_u64().unwrap();
    let info = sw.info().await;
    let per = |a: u64, b: u64| (b - a) as f64 / secs;
    let mut ping = lat.lock().clone();
    ping.sort_by(f64::total_cmp);
    let ping = if ping.is_empty() {
        "none".to_string()
    } else {
        format!("{:.2} / {:.2} / {:.2} ms", q(&ping, 0.5), q(&ping, 0.9), q(&ping, 0.99))
    };
    println!(
        "{n:>5} players: {:>9.0} deliveries/s (of {:>7}), {:>7.0} bundles in/s, {:>8.0} frames routed/s, {:>7.0} messages out/s \
         ({:.1} datagrams each); CPU: switch {:.2} cores ({:.2} user, {:.2} kernel), players {:.2}, process {:.2}; switch ping p50/p90/p99 {ping}; shed {}, limited {}, cuts {}",
        per(d0, d1),
        n * FRIENDS.min(n - 1) * HZ as usize,
        per(s0, s1),
        per(f0, f1),
        per(m0, m1),
        (d1 - d0) as f64 / (m1 - m0).max(1) as f64,
        (cs1.0 + cs1.1 - cs0.0 - cs0.1) / secs,
        (cs1.0 - cs0.0) / secs,
        (cs1.1 - cs0.1) / secs,
        (cp1 - cp0) / secs,
        (all1 - all0) / secs,
        info["shed"],
        info["limited"],
        info["cuts"],
    );
    stop.cancel();
    for t in tasks {
        let _ = t.await;
    }
}

#[test]
#[ignore]
fn free_roam_crowd_on_the_switch() {
    raise_nofile();
    let cores = std::thread::available_parallelism().map_or(4, |n| n.get());
    // NFSU2_LOAD_THREADS: the switch's workers (default: one a core, as the server's runtime)
    let workers = std::env::var("NFSU2_LOAD_THREADS").ok().and_then(|s| s.parse().ok()).unwrap_or(cores);
    let switch_rt =
        tokio::runtime::Builder::new_multi_thread().worker_threads(workers).enable_all().thread_name("lan-switch").build().unwrap();
    let players_rt = tokio::runtime::Builder::new_multi_thread()
        .worker_threads((cores / 2).max(2))
        .enable_all()
        .thread_name("lan-players")
        .build()
        .unwrap();
    println!("{cores} cores; the switch's runtime on {workers} of them, the players' on {}", (cores / 2).max(2));
    let sizes: Vec<usize> = std::env::var("NFSU2_LOAD_N")
        .ok()
        .map(|s| s.split(',').filter_map(|x| x.trim().parse().ok()).collect())
        .unwrap_or_else(|| vec![120, 480, 1000]);
    // NFSU2_LOAD_SECS: how long each size is measured (after 3 s to warm up)
    let secs = std::env::var("NFSU2_LOAD_SECS").ok().and_then(|s| s.parse().ok()).unwrap_or(5);
    for n in sizes {
        let sw = switch_rt.block_on(async { start(LanOptions { max_peers: 8000, ..LanOptions::default() }).await });
        let (lan, port) = (sw.lan.clone(), sw.port);
        players_rt.block_on(crowd(port, n, Duration::from_secs(3), Duration::from_secs(secs), lan));
        switch_rt.block_on(sw.close());
        std::thread::sleep(Duration::from_millis(500));
    }
}
