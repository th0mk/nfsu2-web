//! A crowd of simulated players for the server, light enough that thousands of them leave the cores to the server:
//! what web/tools/roam-crowd.mjs does, without the game's own core (which costs each of its bots a wasm instance).
//! Each bot is a page: one /lan WebSocket (its own client address by X-Forwarded-For, so the per-address limits see
//! as many clients), a hello that takes bundles, and then by --mode:
//!   - roam (the default): free roam with the hub on: a HELLO to the hub every second and a SNAP twice a second
//!     (runtime/roam_core.c's layouts), and to each friend on the hub's last PEERS list a SNAP 20 times a second (and a
//!     HELLO every second), each as one bundle entry for all of them, as the page sends them (web/src/net TxBundle)
//!   - idle: free roam on but not driving (in a race, a shop or the menus): only its HELLO to the hub every second,
//!     with that status, and the page's pings; it holds a place and sends next to nothing
//! What it measures, from the players' side:
//!   - how many friends each has (the hub's lists: 32 when the server keeps up, fewer when its busy cap cuts them)
//!   - snapshots sent (one per friend) and received, so how many got lost on the way (the switch drops a datagram
//!     for a receiver that is too far behind, nfsu2-lan's `roam_queue`)
//!   - a snapshot's delay from sender to friend through the switch (the SNAP's own clock field carries the send
//!     time; one clock for every bot), and the switch's answer to the page's ping
//!   - WebSocket messages and bytes each way (what the server, the tunnel in front and the port pay for)
//!   - that every snapshot arrives as it was sent: a SNAP's state is a function of its sender's number (in its
//!     cfg_rev) and its clock, so the receiver makes it again and compares every byte (bad; with --compact and
//!     --delta, through the compact bundles and their deltas)
//!   - with --queue, the line (nfsu2-lan queue.rs): how long each waited, how close the first wait it was told was, how
//!     many were let in before one that came earlier (overtaken), and whether one that dropped while waiting got its
//!     place back with its ticket
//!
//!   nfsu2-roamload --base http://127.0.0.1:8691 [--n 2000] [--seconds 60] [--rate 20] [--spread 2000] [--room 7]
//!        [--stagger-ms 5] [--first 0] [--threads 4] [--out file.json] [--mode roam|idle] [--compact] [--delta] [--fpred]
//!        [--queue] [--churn 120] [--leave-drop 0.5] [--drop-wait 0.1] [--tag roam]
//!   --rate        snapshots a second to each friend (the game: 20; Settings allow 60)
//!   --spread      the players' area, a square this many metres a side (each drives a circle of 40 m in it); every
//!                 roam generator against one server needs the same (the receivers check the senders' states)
//!   --first       the first bot's number (two generators against one server: --first 0 and --first 1000)
//!   --compact     the hello takes compact bundles (kind 33); --delta deltas in them too; --fpred and friends' car states by
//!                 their fields (nfsu2-lan fpred.rs), rounded: each is checked against its state rounded so (as the page
//!                 does).  A bot's clock goes in steps of 256 µs, as the fields carry the time, so its state comes back
//!   --queue       /lan?queue: a bot waits in line when the server is full (as the page does), else it is refused
//!   --churn       each bot plays about this many seconds (exponential), then leaves (--leave-drop of them by a dropped
//!                 link, which keeps the place a minute, the rest as the page lets go, 1000 "idle") and comes back as
//!                 a new player a moment later; a refused one tries again after 5 s, as the page does
//!   --drop-wait   of the bots waiting in line, this share drop their link once and come back with their ticket
//!   --check-every one snapshot in this many is checked byte for byte (1: all; a crowd of thousands driving: its checks
//!                 would be the generator's limit)
//!   --blip 120:0.4  at 120 s, 40 % of the bots on the switch lose their link at once (as when a Cloudflare Tunnel
//!                 connection goes) and come straight back as the page does: /lan?resume with their address and token
//!                 (blip: how many got their address back, a new one, waited in line, and how long it took)

use std::collections::HashMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicU32, AtomicU64, Ordering};
use std::time::{Duration, Instant};

use bytes::Bytes;
use futures_util::{SinkExt, StreamExt};
use nfsu2_util::Counter;
use parking_lot::Mutex;
use serde_json::Value;
use tokio::time::{MissedTickBehavior, interval_at, sleep};
use tokio_tungstenite::tungstenite::protocol::CloseFrame;
use tokio_tungstenite::tungstenite::{Message, client::IntoClientRequest, http::HeaderValue};

const HUB: u32 = 0x0A4D_00FD;
const PORT: u16 = 47210;
const KIND_UDP: u8 = 1;
const KIND_BUNDLE: u8 = 32;
const KIND_COMPACT: u8 = 33;
const MSG_HELLO: u8 = 1;
const MSG_PEERS: u8 = 2;
const MSG_SNAP: u8 = 3;
const HF_ALL: u16 = 0x0020;
const HF_STATEX: u16 = 0x0040;
const CORE_VER: u16 = 4;
/// roam_core.h RC_ST_ROAM, RC_ST_EVENT
const ST_ROAM: u8 = 2;
const ST_EVENT: u8 = 3;
/// a SNAP's state and its extension, as these bots write them
const STATE: usize = 52 + 12;
/// compact bundles' flags (nfsu2-lan compact.rs)
const C_BCAST: u8 = 0x08;
const C_PORTS: u8 = 0x10;
const C_ROAM: u8 = 0x20;
const C_WHOLE: u8 = 0x40;
const C_DELTA: u8 = 0x80;
const REFS_MAX: usize = 256;

#[derive(Clone, Copy, PartialEq)]
enum Mode {
    Roam,
    Idle,
}

struct Args {
    host: String,
    n: usize,
    seconds: u64,
    rate: f64,
    spread: f64,
    room: u32,
    stagger_ms: u64,
    first: usize,
    threads: usize,
    out: String,
    mode: Mode,
    compact: bool,
    delta: bool,
    fpred: bool,
    queue: bool,
    churn: f64,
    leave_drop: f64,
    drop_wait: f64,
    tag: String,
    blip_at: f64,
    blip_share: f64,
    check_every: u64,
}

fn args() -> Args {
    let a: Vec<String> = std::env::args().collect();
    let opt =
        |k: &str, d: &str| -> String { a.iter().position(|x| x == k).and_then(|i| a.get(i + 1)).cloned().unwrap_or_else(|| d.to_string()) };
    let flag = |k: &str| a.iter().any(|x| x == k);
    let base = opt("--base", "http://127.0.0.1:8080");
    let host = base.trim_start_matches("http://").trim_start_matches("ws://").trim_end_matches('/').to_string();
    let fpred = flag("--fpred");
    let delta = fpred || flag("--delta");
    Args {
        host,
        n: opt("--n", "2000").parse().expect("--n"),
        seconds: opt("--seconds", "60").parse().expect("--seconds"),
        rate: opt("--rate", "20").parse().expect("--rate"),
        spread: opt("--spread", "2000").parse().expect("--spread"),
        room: opt("--room", "7").parse().expect("--room"),
        stagger_ms: opt("--stagger-ms", "5").parse().expect("--stagger-ms"),
        first: opt("--first", "0").parse().expect("--first"),
        threads: opt("--threads", "4").parse().expect("--threads"),
        out: opt("--out", ""),
        mode: match opt("--mode", "roam").as_str() {
            "idle" => Mode::Idle,
            _ => Mode::Roam,
        },
        compact: delta || flag("--compact"),
        delta,
        fpred,
        queue: flag("--queue"),
        churn: opt("--churn", "0").parse().expect("--churn"),
        leave_drop: opt("--leave-drop", "0.5").parse().expect("--leave-drop"),
        drop_wait: opt("--drop-wait", "0").parse().expect("--drop-wait"),
        tag: opt("--tag", ""),
        blip_at: opt("--blip", "0:0").split(':').next().unwrap_or("0").parse().expect("--blip"),
        blip_share: opt("--blip", "0:0").split(':').nth(1).unwrap_or("0").parse().expect("--blip"),
        check_every: opt("--check-every", "1").parse().expect("--check-every"),
    }
}

/// A histogram of microseconds: 10 µs steps below 1 ms, 100 µs to 10 ms, 1 ms to 100 ms, 10 ms to 1 s, 100 ms to
/// 10 s, one bucket beyond.
const BUCKETS: usize = 461;
fn bucket(us: u64) -> usize {
    match us {
        0..1_000 => (us / 10) as usize,
        1_000..10_000 => 100 + ((us - 1_000) / 100) as usize,
        10_000..100_000 => 190 + ((us - 10_000) / 1_000) as usize,
        100_000..1_000_000 => 280 + ((us - 100_000) / 10_000) as usize,
        1_000_000..10_000_000 => 370 + ((us - 1_000_000) / 100_000) as usize,
        _ => 460,
    }
}
/// a bucket's upper end, in ms
fn bucket_ms(b: usize) -> f64 {
    let us = match b {
        0..100 => (b + 1) * 10,
        100..190 => 1_000 + (b - 99) * 100,
        190..280 => 10_000 + (b - 189) * 1_000,
        280..370 => 100_000 + (b - 279) * 10_000,
        370..460 => 1_000_000 + (b - 369) * 100_000,
        _ => 10_000_000,
    };
    us as f64 / 1000.0
}
fn pct(h: &[u64], p: f64) -> Option<f64> {
    let n: u64 = h.iter().sum();
    if n == 0 {
        return None;
    }
    let want = ((p / 100.0) * n as f64).ceil().max(1.0) as u64;
    let mut acc = 0;
    for (b, c) in h.iter().enumerate() {
        acc += c;
        if acc >= want {
            return Some(bucket_ms(b));
        }
    }
    Some(bucket_ms(BUCKETS - 1))
}
/// the p-th percentile of a list (nearest rank), to a tenth
fn pct_of(v: &mut [f64], p: f64) -> Option<f64> {
    if v.is_empty() {
        return None;
    }
    v.sort_by(|a, b| a.total_cmp(b));
    let k = ((p / 100.0) * v.len() as f64).ceil().max(1.0) as usize - 1;
    Some((v[k.min(v.len() - 1)] * 10.0).round() / 10.0)
}

#[derive(Default)]
struct Hists {
    lat: Vec<u64>,
    ping: Vec<u64>,
}

impl Hists {
    fn new() -> Self {
        Hists { lat: vec![0; BUCKETS], ping: vec![0; BUCKETS] }
    }
}

/// One wait in line: when the bot was first told its place and when it was let in (ms since the start), whether it
/// dropped its link meanwhile, the wait it was told first (s)
struct Wait {
    told: f64,
    went_in: f64,
    dropped: bool,
    eta: Option<f64>,
}

struct Shared {
    t0: Instant,
    stop: AtomicBool,
    welcomed: AtomicU64,
    refused: AtomicU64,
    closed: AtomicU64,
    failed: AtomicU64,
    /// players on the switch now, waiting in line now, lives begun, lives ended by churn
    on: AtomicI64,
    waiting: AtomicI64,
    lives: AtomicU64,
    left: AtomicU64,
    msgs_out: Counter,
    msgs_in: Counter,
    bytes_out: Counter,
    bytes_in: Counter,
    compact_in: Counter,
    deltas_in: Counter,
    fpred_in: Counter,
    deliveries: Counter,
    snaps_out: Counter,
    dgrams_in: Counter,
    snaps_in: Counter,
    checked: Counter,
    bad: Counter,
    /// the first bad ones, what was wrong
    bad_log: Mutex<Vec<String>>,
    peers: Vec<AtomicU32>,
    window: Mutex<Hists>,
    total: Mutex<Hists>,
    waits: Mutex<Vec<Wait>>,
    rejoined_same: AtomicU64,
    rejoined_lost: AtomicU64,
    /// the blip: dropped, back at the same address, at a new one, through the line, not back; how long it took (ms)
    blip: [AtomicU64; 5],
    blip_ms: Mutex<Vec<f64>>,
}

impl Shared {
    fn bad(&self, why: impl FnOnce() -> String) {
        self.bad.inc();
        let mut l = self.bad_log.lock();
        if l.len() < 30 {
            l.push(why());
        }
    }
    /// the bots' clock (µs, in steps of 256: what the fields of a car state carry, so that a state rounded by them
    /// is the state of the same time)
    fn us(&self) -> u32 {
        self.t0.elapsed().as_micros() as u32 & !0xFF
    }
    fn ms(&self) -> f64 {
        self.t0.elapsed().as_secs_f64() * 1000.0
    }
}

fn hdr(w: &mut Vec<u8>, kind: u8, sender: u32, seq: u16, flags: u16) {
    w.extend_from_slice(b"N2FR");
    w.push(1);
    w.push(kind);
    w.extend_from_slice(&[0, 0]);
    w.extend_from_slice(&sender.to_le_bytes());
    w.extend_from_slice(&seq.to_le_bytes());
    w.extend_from_slice(&flags.to_le_bytes());
}
fn finish(mut w: Vec<u8>) -> Vec<u8> {
    let len = (w.len() + 4) as u16;
    w[6..8].copy_from_slice(&len.to_le_bytes());
    let crc = crc32fast::hash(&w);
    w.extend_from_slice(&crc.to_le_bytes());
    w
}
fn hello(id: u32, seq: u16, name: &str, room: u32, status: u8) -> Vec<u8> {
    let mut w = Vec::with_capacity(64);
    hdr(&mut w, MSG_HELLO, id, seq, HF_ALL | HF_STATEX);
    w.extend_from_slice(&CORE_VER.to_le_bytes());
    w.push(0); // platform
    w.push(status);
    w.extend_from_slice(&PORT.to_le_bytes());
    w.extend_from_slice(&1u16.to_le_bytes()); // cfg_rev
    w.extend_from_slice(&room.to_le_bytes());
    w.push(name.len() as u8);
    w.extend_from_slice(name.as_bytes());
    w.push(0); // ids seen
    finish(w)
}

/// where bot n drives: the centre of its circle and where on it it starts (a fixed scatter over the square)
fn place(n: usize, spread: f64) -> (f64, f64, f64) {
    let h = |k: u64| {
        ((n as u64 + 1).wrapping_mul(0x9E37_79B9_7F4A_7C15).wrapping_add(k).wrapping_mul(0xBF58_476D_1CE4_E5B9) >> 11) as f64
            / (1u64 << 53) as f64
    };
    ((h(1) - 0.5) * spread, (h(2) - 0.5) * spread, h(3) * std::f64::consts::TAU)
}

/// bot n's state at its clock t_us, as its SNAP carries it: a car going round its circle of 40 m at 15 m/s, its
/// speed, heading, turn, engine and suspension changing as a driving car's do (so that what changes from one snapshot
/// to the next is about what a real one's is), and its extension
fn state(n: usize, t_us: u32, spread: f64) -> [u8; STATE] {
    let (cx, cy, ph) = place(n, spread);
    let t = t_us as f64 / 1e6;
    let a = ph + t * 15.0 / 40.0;
    let mut s = [0u8; STATE];
    let mut w = 0;
    let mut put = |b: &[u8]| {
        s[w..w + b.len()].copy_from_slice(b);
        w += b.len();
    };
    put(&t_us.to_le_bytes());
    for x in [cx + 40.0 * a.cos(), cy + 40.0 * a.sin(), 10.0 + 0.05 * (t * 3.0).sin()] {
        put(&(x as f32).to_le_bytes());
    }
    for x in [-15.0 * a.sin(), 15.0 * a.cos(), 0.15 * (t * 3.0).cos()] {
        put(&(x as f32).to_le_bytes());
    }
    let yaw = a + std::f64::consts::FRAC_PI_2;
    for x in [0.01 * (t * 5.0).sin(), 0.0, (yaw / 2.0).sin(), (yaw / 2.0).cos()] {
        put(&(x as f32).to_le_bytes());
    }
    put(&((375.0 + 20.0 * (t * 2.0).sin()) as i16).to_le_bytes()); // yaw rate (1/1000 rad/s)
    put(&((4000.0 + 1500.0 * (t * 0.7 + ph).sin()) as u16).to_le_bytes()); // rpm
    put(&((1500.0 + 50.0 * (t * 1.3).sin()) as i16).to_le_bytes()); // speed (1/100 m/s)
    put(&[3, 0]); // gear, bits
    // the extension: its length (what follows it), the wheels' heights (mm), nitrous, hydraulics, landings
    put(&[11]);
    for k in 0..4 {
        put(&((-300.0 + 12.0 * (t * 5.0 + k as f64).sin()) as i16).to_le_bytes());
    }
    put(&[0, 0, 0]);
    s
}

/// a SNAP of one state (with its extension for a friend, without for the hub), sent at its clock t_us; cfg_rev
/// carries the bot's number, so that a friend can make the state again
fn snap(id: u32, seq: u16, n: usize, t_us: u32, spread: f64, statex: bool) -> Vec<u8> {
    let mut w = Vec::with_capacity(112);
    hdr(&mut w, MSG_SNAP, id, seq, if statex { HF_STATEX } else { 0 });
    w.extend_from_slice(&[ST_ROAM, 0]);
    w.extend_from_slice(&(n as u16).to_le_bytes());
    w.extend_from_slice(&[1, 0]); // n = 1, clock
    let s = state(n, t_us, spread);
    w.extend_from_slice(if statex { &s[..] } else { &s[..52] });
    finish(w)
}
/// a switch frame of kind 1 around a datagram
fn frame(dst: u32, dgram: &[u8]) -> Vec<u8> {
    let mut f = Vec::with_capacity(16 + dgram.len());
    f.push(KIND_UDP);
    f.extend_from_slice(&[0, 0, 0]);
    f.extend_from_slice(&0u32.to_be_bytes());
    f.extend_from_slice(&dst.to_be_bytes());
    f.extend_from_slice(&PORT.to_be_bytes());
    f.extend_from_slice(&PORT.to_be_bytes());
    f.extend_from_slice(dgram);
    f
}
/// a bundle entry: the frame to each of `to` (none: to its own destination)
fn entry(b: &mut Vec<u8>, to: &[u32], f: &[u8]) {
    b.push(to.len() as u8);
    b.push(0);
    b.extend_from_slice(&(f.len() as u16).to_be_bytes());
    for v in to {
        b.extend_from_slice(&v.to_be_bytes());
    }
    b.extend_from_slice(f);
}

struct Bot {
    i: usize,
    n: usize,
    id: u32,
    name: String,
    room: u32,
    seq: u16,
    spread: f64,
    friends: Vec<u32>,
    lat: Vec<u64>,
    ping: Vec<u64>,
    pings: Vec<(u64, Instant)>,
    ping_id: u64,
    /// the last body of each source's free-roam datagrams of each type in compact bundles (the page's `refs`)
    refs: HashMap<u32, Vec<u8>>,
    /// friends' last car states, for those by their fields (the page's `fp`)
    fp: nfsu2_lan::fpred::Fpred,
    /// snapshots taken, and every how many one is checked byte for byte (--check-every)
    snaps_seen: u64,
    check_every: u64,
}

impl Bot {
    fn next_seq(&mut self) -> u16 {
        self.seq = self.seq.wrapping_add(1);
        self.seq
    }
    /// one free-roam datagram from src
    fn take(&mut self, sh: &Shared, src: u32, m: &[u8], via: &str) {
        sh.dgrams_in.inc();
        if m.len() < 20 || &m[0..4] != b"N2FR" {
            return;
        }
        match m[5] {
            MSG_PEERS if src == HUB && m.len() >= 21 => {
                let n = m[16] as usize;
                if m.len() < 17 + 10 * n + 4 {
                    return;
                }
                self.friends.clear();
                for k in 0..n {
                    let o = 17 + 10 * k;
                    self.friends.push(u32::from_be_bytes([m[o], m[o + 1], m[o + 2], m[o + 3]]));
                }
                sh.peers[self.i].store(n as u32, Ordering::Relaxed);
            }
            MSG_SNAP if m.len() >= 26 => {
                sh.snaps_in.inc();
                let t = u32::from_le_bytes([m[22], m[23], m[24], m[25]]);
                let d = sh.us().wrapping_sub(t) as u64;
                // a wrapped or future clock (cannot happen within one process) is left out
                if d < 3_600_000_000 {
                    self.lat[bucket(d)] += 1;
                }
                // its state, byte for byte, as its sender made it
                self.snaps_seen = self.snaps_seen.wrapping_add(1);
                if u16::from_le_bytes([m[14], m[15]]) & HF_STATEX != 0 && m.len() >= 22 + STATE && self.snaps_seen % self.check_every == 0 {
                    sh.checked.inc();
                    let n = u16::from_le_bytes([m[18], m[19]]) as usize;
                    let mut want = state(n, t, self.spread);
                    if via == "fpred" {
                        // rounded as the fields round it (nfsu2-lan fpred.rs): the state in a body of its own
                        let mut b = vec![0u8; 14];
                        b[6..8].copy_from_slice(&HF_STATEX.to_le_bytes());
                        b[12] = 1;
                        b.extend_from_slice(&want);
                        match nfsu2_lan::fpred::rounded(&b) {
                            Some(r) => want.copy_from_slice(&r[14..14 + STATE]),
                            None => sh.bad(|| format!("a state by its fields the switch could not have made: n {n} t {t}")),
                        }
                    }
                    if m[22..22 + STATE] != want {
                        let at = (0..STATE).find(|&k| m[22 + k] != want[k]).unwrap_or(0);
                        sh.bad(|| format!("state via {via} from {src:08x} n {n} t {t} len {}: byte {at} got {:02x?} want {:02x?}", m.len(),
                            &m[22 + at..(22 + at + 6).min(22 + STATE)], &want[at..(at + 6).min(STATE)]));
                    }
                }
            }
            _ => {}
        }
    }
    /// one switch frame from the server
    fn take_frame(&mut self, sh: &Shared, f: &[u8]) {
        if f.len() < 16 + 20 || f[0] != KIND_UDP {
            return;
        }
        self.take(sh, u32::from_be_bytes([f[4], f[5], f[6], f[7]]), &f[16..], "frame");
    }
    /// a compact bundle (nfsu2-lan compact.rs, web/src/net/proto.ts unpackCompact): each free-roam datagram put
    /// together again (all but its CRC, which these bots do not read); a broken entry counts as bad and ends it
    fn compact(&mut self, sh: &Shared, u: &[u8]) {
        sh.compact_in.inc();
        let mut at = 4;
        while at + 5 <= u.len() {
            let flags = u[at];
            if flags == nfsu2_lan::fpred::F_FPRED {
                // a friend's car state by its fields: its body as the page puts it back
                let src = u16::from_be_bytes([u[at + 1], u[at + 2]]);
                let end = at + 4 + u[at + 3] as usize;
                if end > u.len() {
                    sh.bad(|| format!("a state by its fields past the bundle: end {end} of {}", u.len()));
                    return;
                }
                let Some(body) = self.fp.decode(src, &u[at + 4..end]) else {
                    sh.bad(|| format!("a state by its fields that does not decode: from {src:04x}, {} bytes", end - at));
                    return;
                };
                at = end;
                sh.fpred_in.inc();
                let mut m = Vec::with_capacity(body.len() + 12);
                m.extend_from_slice(b"N2FR");
                m.extend_from_slice(&[1, MSG_SNAP]);
                m.extend_from_slice(&((body.len() + 12) as u16).to_le_bytes());
                m.extend_from_slice(&body);
                m.extend_from_slice(&[0; 4]);
                self.take(sh, 0x0A4D_0000 | src as u32, &m, "fpred");
                continue;
            }
            let src = 0x0A4D_0000 | u16::from_be_bytes([u[at + 1], u[at + 2]]) as u32;
            let end = at + 5 + u16::from_be_bytes([u[at + 3], u[at + 4]]) as usize;
            if end > u.len() {
                sh.bad(|| format!("entry past the bundle: flags {flags:02x} end {end} of {}", u.len()));
                return;
            }
            let p = at + 5;
            at = end;
            if flags == C_WHOLE {
                self.take_frame(sh, &u[p..end]);
                continue;
            }
            let d = p + if flags & C_PORTS != 0 { 4 } else { 0 };
            if flags & 7 != KIND_UDP || flags & C_ROAM == 0 || end < d + 1 {
                continue;
            }
            let kind = u[d];
            let key = (src & 0xFFFF) << 8 | kind as u32;
            let body = if flags & C_DELTA != 0 {
                let Some(last) = self.refs.get(&key) else {
                    let k = self.refs.len();
                    sh.bad(|| format!("delta without a last body: key {key:08x} flags {flags:02x} len {} refs {k}", end - d));
                    return;
                };
                let map = last.len().div_ceil(8);
                if d + 1 + map > end {
                    sh.bad(|| format!("delta shorter than its bitmap: key {key:08x} len {} last {}", end - d, last.len()));
                    return;
                }
                let mut next = d + 1 + map;
                let mut body = last.clone();
                for (i, b) in body.iter_mut().enumerate() {
                    if u[d + 1 + i / 8] >> (i % 8) & 1 == 1 {
                        if next >= end {
                            sh.bad(|| format!("delta's new bytes past its end: key {key:08x}"));
                            return;
                        }
                        *b = u[next];
                        next += 1;
                    }
                }
                if next != end {
                    sh.bad(|| format!("delta's new bytes short of its end: key {key:08x} {next} of {end}"));
                    return;
                }
                sh.deltas_in.inc();
                body
            } else {
                u[d + 1..end].to_vec()
            };
            let mut m = Vec::with_capacity(body.len() + 12);
            m.extend_from_slice(b"N2FR");
            m.extend_from_slice(&[1, kind]);
            m.extend_from_slice(&((body.len() + 12) as u16).to_le_bytes());
            m.extend_from_slice(&body);
            m.extend_from_slice(&[0; 4]);
            if flags & (C_PORTS | C_BCAST) == 0 {
                if self.refs.len() >= REFS_MAX && !self.refs.contains_key(&key) {
                    self.refs.clear();
                }
                self.refs.insert(key, body);
            }
            self.take(sh, src, &m, if flags & C_DELTA != 0 { "delta" } else { "compact" });
        }
    }
    fn message(&mut self, sh: &Shared, data: &[u8]) {
        if data.is_empty() {
            return;
        }
        if data[0] == KIND_COMPACT {
            return self.compact(sh, data);
        }
        if data[0] != KIND_BUNDLE {
            return self.take_frame(sh, data);
        }
        let mut at = 4;
        while at + 4 <= data.len() {
            let k = data[at] as usize;
            let len = u16::from_be_bytes([data[at + 2], data[at + 3]]) as usize;
            let f = at + 4 + 4 * k;
            if f + len > data.len() {
                return;
            }
            self.take_frame(sh, &data[f..f + len]);
            at = f + len;
        }
    }
    fn flush_hists(&mut self, sh: &Shared) {
        let mut w = sh.window.lock();
        for (a, b) in w.lat.iter_mut().zip(&self.lat) {
            *a += b;
        }
        for (a, b) in w.ping.iter_mut().zip(&self.ping) {
            *a += b;
        }
        drop(w);
        let mut t = sh.total.lock();
        for (a, b) in t.lat.iter_mut().zip(&self.lat) {
            *a += b;
        }
        for (a, b) in t.ping.iter_mut().zip(&self.ping) {
            *a += b;
        }
        self.lat.iter_mut().for_each(|x| *x = 0);
        self.ping.iter_mut().for_each(|x| *x = 0);
    }
}

type Ws = tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;
type Tx = futures_util::stream::SplitSink<Ws, Message>;
type Rx = futures_util::stream::SplitStream<Ws>;

/// How a life ended.
#[derive(PartialEq)]
enum End {
    /// the run is over
    Stop,
    /// not let in: "LAN full" (a full line, or a bot that cannot wait), or no connection
    Refused,
    /// it left (churn) or its connection went
    Left,
}

async fn connect(a: &Args, n: usize, resume: bool) -> Option<(Tx, Rx)> {
    let q = match (a.queue, resume) {
        (true, true) => "?queue&resume",
        (true, false) => "?queue",
        (false, true) => "?resume",
        (false, false) => "",
    };
    let mut req = format!("ws://{}/lan{q}", a.host).into_client_request().expect("url");
    let xff = format!("198.18.{}.{}", (n >> 8) & 255, n & 255);
    req.headers_mut().insert("x-forwarded-for", HeaderValue::from_str(&xff).expect("xff"));
    tokio_tungstenite::connect_async(req).await.ok().map(|(ws, _)| ws.split())
}

fn hello_text(name: &str, a: &Args, ticket: Option<&str>) -> String {
    let mut h = serde_json::json!({ "t": "hello", "name": name, "deltas": 1, "lan": 0, "bundle": 1 });
    if a.compact {
        h["compact"] = 1.into();
    }
    if a.delta {
        h["delta"] = 1.into();
    }
    if a.fpred {
        h["fpred"] = 1.into();
    }
    if let Some(t) = ticket {
        h["ticket"] = t.into();
    }
    h.to_string()
}

/// One life of bot i: connect (waiting in line when the server is full and the bot can), play until `life` is over,
/// leave.
async fn run_life(i: usize, a: &Args, sh: &Arc<Shared>, life: Duration) -> End {
    let n = a.first + i;
    let name = format!("bot{n}");
    sh.lives.fetch_add(1, Ordering::Relaxed);
    let Some((mut tx, mut rx)) = connect(a, n, false).await else {
        sh.failed.fetch_add(1, Ordering::Relaxed);
        return End::Refused;
    };
    // a page that can wait says hello at once (its place in line keeps it); one of before after its welcome
    if a.queue && tx.send(Message::Text(hello_text(&name, a, None).into())).await.is_err() {
        return End::Refused;
    }
    // the welcome, perhaps after a wait in line
    let mut told: Option<(f64, Option<f64>)> = None;
    let mut ticket = String::new();
    let mut pos = 0u64;
    let mut dropped = false;
    let mut held = (0u32, String::new());
    let mut drop_at = (a.drop_wait > 0.0 && nfsu2_util::rand::unit() < a.drop_wait)
        .then(|| tokio::time::Instant::now() + Duration::from_secs_f64(2.0 + 20.0 * nfsu2_util::rand::unit()));
    let let_go = |told: &Option<(f64, Option<f64>)>| {
        if told.is_some() {
            sh.waiting.fetch_sub(1, Ordering::Relaxed);
        }
    };
    loop {
        if sh.stop.load(Ordering::Relaxed) {
            let_go(&told);
            return End::Stop;
        }
        let m = tokio::select! {
            m = rx.next() => m,
            // the page pings every 2 s while it is connected, also while it waits (one that sends nothing for a minute
            // has gone, for the line)
            _ = tokio::time::sleep(Duration::from_secs(2)) => {
                if tx.send(Message::Text(r#"{"t":"ping","id":0}"#.into())).await.is_err() {
                    let_go(&told);
                    sh.refused.fetch_add(1, Ordering::Relaxed);
                    return End::Refused;
                }
                continue;
            }
            _ = tokio::time::sleep_until(drop_at.unwrap_or_else(tokio::time::Instant::now)), if drop_at.is_some() && told.is_some() => {
                // its link drops while it waits: back with its ticket, at its place
                drop_at = None;
                dropped = true;
                drop((tx, rx));
                let Some((t2, r2)) = connect(a, n, true).await else {
                    let_go(&told);
                    sh.failed.fetch_add(1, Ordering::Relaxed);
                    return End::Refused;
                };
                (tx, rx) = (t2, r2);
                if tx.send(Message::Text(hello_text(&name, a, Some(&ticket)).into())).await.is_err() {
                    let_go(&told);
                    return End::Refused;
                }
                // its place, as the line tells it now (0: let in at once)
                let back = loop {
                    match rx.next().await {
                        Some(Ok(Message::Text(t))) => {
                            let Ok(v) = serde_json::from_str::<Value>(t.as_str()) else { continue };
                            if v["t"] == "queue" { break v["pos"].as_u64(); }
                            if v["t"] == "welcome" {
                                held = (v["vip"].as_u64().unwrap_or(0) as u32, v["token"].as_str().unwrap_or("").to_string());
                                break Some(0);
                            }
                        }
                        Some(Ok(Message::Binary(_))) => sh.bad(|| "a binary message before the welcome (back with a ticket)".into()),
                        Some(Ok(_)) => {}
                        _ => break None,
                    }
                };
                match back {
                    Some(p) if p <= pos => {
                        sh.rejoined_same.fetch_add(1, Ordering::Relaxed);
                        if p == 0 {
                            break;
                        }
                        pos = p;
                    }
                    Some(p) => {
                        sh.rejoined_lost.fetch_add(1, Ordering::Relaxed);
                        pos = p;
                    }
                    None => {
                        let_go(&told);
                        sh.refused.fetch_add(1, Ordering::Relaxed);
                        return End::Refused;
                    }
                }
                continue;
            }
        };
        match m {
            Some(Ok(Message::Text(t))) => {
                let Ok(v) = serde_json::from_str::<Value>(t.as_str()) else { continue };
                if v["t"] == "welcome" {
                    held = (v["vip"].as_u64().unwrap_or(0) as u32, v["token"].as_str().unwrap_or("").to_string());
                    break;
                }
                if v["t"] == "queue" {
                    if told.is_none() {
                        told = Some((sh.ms(), v["eta"].as_f64()));
                        sh.waiting.fetch_add(1, Ordering::Relaxed);
                    }
                    if let Some(t) = v["ticket"].as_str() {
                        ticket = t.to_string();
                    }
                    pos = v["pos"].as_u64().unwrap_or(pos);
                }
            }
            Some(Ok(Message::Close(_))) | None | Some(Err(_)) => {
                let_go(&told);
                sh.refused.fetch_add(1, Ordering::Relaxed);
                return End::Refused;
            }
            // (the switch sends a page nothing but text before its welcome)
            Some(Ok(Message::Binary(_))) => sh.bad(|| "a binary message before the welcome".into()),
            _ => {}
        }
    }
    if let Some((at, eta)) = told {
        sh.waiting.fetch_sub(1, Ordering::Relaxed);
        sh.waits.lock().push(Wait { told: at, went_in: sh.ms(), dropped, eta });
    }
    sh.welcomed.fetch_add(1, Ordering::Relaxed);
    sh.on.fetch_add(1, Ordering::Relaxed);
    let end = if !a.queue && tx.send(Message::Text(hello_text(&name, a, None).into())).await.is_err() {
        sh.closed.fetch_add(1, Ordering::Relaxed);
        End::Left
    } else {
        play(i, n, a, sh, tx, rx, life, name, held).await
    };
    sh.on.fetch_sub(1, Ordering::Relaxed);
    sh.peers[i].store(0, Ordering::Relaxed);
    end
}

/// a bot on the switch until its life is over (or the run, or its connection)
#[allow(clippy::too_many_arguments)]
async fn play(
    i: usize,
    n: usize,
    a: &Args,
    sh: &Arc<Shared>,
    mut tx: Tx,
    mut rx: Rx,
    life: Duration,
    name: String,
    mut held: (u32, String),
) -> End {
    let mut bot = Bot {
        i,
        n,
        id: (nfsu2_util::rand::u32() | 1),
        name,
        room: a.room,
        seq: 0,
        spread: a.spread,
        friends: Vec::new(),
        lat: vec![0; BUCKETS],
        ping: vec![0; BUCKETS],
        pings: Vec::new(),
        ping_id: 0,
        refs: HashMap::new(),
        fp: Default::default(),
        snaps_seen: 0,
        check_every: a.check_every.max(1),
    };
    let roam = a.mode == Mode::Roam;
    let status = if roam { ST_ROAM } else { ST_EVENT };
    let now = tokio::time::Instant::now();
    let until = now + life;
    // the blip, for this bot when it is on the switch then
    let blip_in = a.blip_at - sh.t0.elapsed().as_secs_f64();
    let mut blip = (a.blip_at > 0.0 && blip_in > 0.0 && nfsu2_util::rand::unit() < a.blip_share)
        .then(|| now + Duration::from_secs_f64(blip_in));
    let period = Duration::from_secs_f64(1.0 / a.rate);
    let jitter = |max: Duration| Duration::from_secs_f64(nfsu2_util::rand::unit() * max.as_secs_f64());
    let mut snap_iv = interval_at(now + jitter(period), period);
    let mut hello_iv = interval_at(now + jitter(Duration::from_secs(1)), Duration::from_secs(1));
    let mut hub_iv = interval_at(now + jitter(Duration::from_millis(500)), Duration::from_millis(500));
    let mut ping_iv = interval_at(now + jitter(Duration::from_secs(2)), Duration::from_secs(2));
    let mut flush_iv = interval_at(now + jitter(Duration::from_secs(1)), Duration::from_secs(1));
    for iv in [&mut snap_iv, &mut hello_iv, &mut hub_iv, &mut ping_iv, &mut flush_iv] {
        iv.set_missed_tick_behavior(MissedTickBehavior::Delay);
    }
    // the first HELLO at once: the hub starts a player with it
    let first = frame(HUB, &hello(bot.id, 0, &bot.name, bot.room, status));
    let mut b = vec![KIND_BUNDLE, 0, 0, 0];
    entry(&mut b, &[], &first);
    sh.bytes_out.add(b.len() as u64);
    let _ = tx.send(Message::Binary(Bytes::from(b))).await;
    sh.msgs_out.inc();
    let end = loop {
        if sh.stop.load(Ordering::Relaxed) {
            let _ = tx.send(Message::Close(None)).await;
            break End::Stop;
        }
        let out: Option<Vec<u8>> = tokio::select! {
            m = rx.next() => match m {
                Some(Ok(Message::Binary(d))) => { sh.msgs_in.inc(); sh.bytes_in.add(d.len() as u64); bot.message(sh, &d); None }
                Some(Ok(Message::Text(t))) => {
                    sh.msgs_in.inc();
                    sh.bytes_in.add(t.len() as u64);
                    let t = t.as_str();
                    if let Some(p) = t.find("\"pong\"").and(t.find("\"id\":")) {
                        let id: u64 = t[p + 5..].trim_end_matches('}').parse().unwrap_or(u64::MAX);
                        if let Some(k) = bot.pings.iter().position(|(x, _)| *x == id) {
                            let (_, at) = bot.pings.swap_remove(k);
                            bot.ping[bucket(at.elapsed().as_micros() as u64)] += 1;
                        }
                    }
                    None
                }
                Some(Ok(_)) => None,
                Some(Err(_)) | None => { sh.closed.fetch_add(1, Ordering::Relaxed); break End::Left; }
            },
            _ = tokio::time::sleep_until(blip.unwrap_or(until)), if blip.is_some() => {
                // its link goes, and it comes back at once as the page does: ?resume, its address and token first
                blip = None;
                sh.blip[0].fetch_add(1, Ordering::Relaxed);
                let t = Instant::now();
                drop((tx, rx));
                // as the page: again after 250 ms, twice as long each time up to 5 s, while its address is kept (60 s)
                let mut back_off = 0.25;
                let (t2, mut r2) = loop {
                    if let Some(c) = connect(a, n, true).await {
                        break c;
                    }
                    if t.elapsed() > Duration::from_secs(60) || sh.stop.load(Ordering::Relaxed) {
                        sh.blip[4].fetch_add(1, Ordering::Relaxed);
                        return End::Left;
                    }
                    sleep(Duration::from_secs_f64(back_off)).await;
                    back_off = (back_off * 2.0).min(5.0);
                };
                tx = t2;
                let mut h = serde_json::from_str::<Value>(&hello_text(&bot.name, a, None)).unwrap();
                h["vip"] = format!("10.77.{}.{}", (held.0 >> 8) & 255, held.0 & 255).into();
                h["token"] = held.1.clone().into();
                if tx.send(Message::Text(h.to_string().into())).await.is_err() {
                    sh.blip[4].fetch_add(1, Ordering::Relaxed);
                    return End::Left;
                }
                let mut queued = false;
                let back = loop {
                    match r2.next().await {
                        Some(Ok(Message::Text(m))) => {
                            let Ok(v) = serde_json::from_str::<Value>(m.as_str()) else { continue };
                            if v["t"] == "queue" { queued = true; }
                            if v["t"] == "welcome" { break Some((v["vip"].as_u64().unwrap_or(0) as u32, v["token"].as_str().unwrap_or("").to_string())); }
                        }
                        Some(Ok(_)) => {}
                        _ => break None,
                    }
                };
                rx = r2;
                bot.refs.clear();
                match back {
                    Some(h2) => {
                        sh.blip[if queued { 3 } else if h2.0 == held.0 { 1 } else { 2 }].fetch_add(1, Ordering::Relaxed);
                        sh.blip_ms.lock().push(t.elapsed().as_secs_f64() * 1000.0);
                        held = h2;
                        None
                    }
                    None => { sh.blip[4].fetch_add(1, Ordering::Relaxed); return End::Left; }
                }
            }
            _ = tokio::time::sleep_until(until) => {
                // its life is over: a dropped link (its place kept a minute) or the page letting go
                sh.left.fetch_add(1, Ordering::Relaxed);
                if nfsu2_util::rand::unit() >= a.leave_drop {
                    let _ = tx.send(Message::Close(Some(CloseFrame { code: 1000.into(), reason: "idle".into() }))).await;
                }
                break End::Left;
            }
            _ = snap_iv.tick(), if roam => {
                if bot.friends.is_empty() { None } else {
                    let seq = bot.next_seq();
                    let f = frame(0, &snap(bot.id, seq, bot.n, sh.us(), bot.spread, true));
                    let mut b = Vec::with_capacity(8 + 4 * bot.friends.len() + f.len());
                    b.extend_from_slice(&[KIND_BUNDLE, 0, 0, 0]);
                    entry(&mut b, &bot.friends, &f);
                    sh.deliveries.add(bot.friends.len() as u64);
                    sh.snaps_out.add(bot.friends.len() as u64);
                    Some(b)
                }
            }
            _ = hello_iv.tick() => {
                let seq = bot.next_seq();
                let hub = frame(HUB, &hello(bot.id, seq, &bot.name, bot.room, status));
                let mut b = vec![KIND_BUNDLE, 0, 0, 0];
                entry(&mut b, &[], &hub);
                if roam && !bot.friends.is_empty() {
                    let f = frame(0, &hello(bot.id, seq, &bot.name, bot.room, status));
                    entry(&mut b, &bot.friends, &f);
                    sh.deliveries.add(bot.friends.len() as u64);
                }
                Some(b)
            }
            _ = hub_iv.tick(), if roam => {
                let seq = bot.next_seq();
                Some(frame(HUB, &snap(bot.id, seq, bot.n, sh.us(), bot.spread, false)))
            }
            _ = ping_iv.tick() => {
                bot.ping_id += 1;
                bot.pings.push((bot.ping_id, Instant::now()));
                if bot.pings.len() > 16 { bot.pings.remove(0); }
                let m = format!("{{\"t\":\"ping\",\"id\":{}}}", bot.ping_id);
                sh.msgs_out.inc();
                sh.bytes_out.add(m.len() as u64);
                if tx.send(Message::Text(m.into())).await.is_err() { sh.closed.fetch_add(1, Ordering::Relaxed); break End::Left; }
                None
            }
            _ = flush_iv.tick() => { bot.flush_hists(sh); None }
        };
        if let Some(b) = out {
            sh.msgs_out.inc();
            sh.bytes_out.add(b.len() as u64);
            if tx.send(Message::Binary(Bytes::from(b))).await.is_err() {
                sh.closed.fetch_add(1, Ordering::Relaxed);
                break End::Left;
            }
        }
    };
    bot.flush_hists(sh);
    end
}

/// bot i: one life, or with --churn one after another until the run ends
async fn run_bot(i: usize, a: Arc<Args>, sh: Arc<Shared>) {
    loop {
        let life = if a.churn > 0.0 {
            Duration::from_secs_f64(-a.churn * (1.0 - nfsu2_util::rand::unit()).ln())
        } else {
            Duration::from_secs(1 << 30)
        };
        let end = run_life(i, &a, &sh, life).await;
        if end == End::Stop || a.churn <= 0.0 || sh.stop.load(Ordering::Relaxed) {
            return;
        }
        // a refused page asks again after 5 s; one that left is a new player a moment later
        let pause = if end == End::Refused { 5.0 } else { 2.0 * nfsu2_util::rand::unit() };
        sleep(Duration::from_secs_f64(pause)).await;
    }
}

fn raise_nofile() {
    // SAFETY: plain libc calls on a struct they fill
    unsafe {
        let mut r: libc::rlimit = std::mem::zeroed();
        if libc::getrlimit(libc::RLIMIT_NOFILE, &mut r) == 0 && r.rlim_cur < r.rlim_max {
            r.rlim_cur = r.rlim_max.min(1 << 20);
            libc::setrlimit(libc::RLIMIT_NOFILE, &r);
        }
    }
}

/// the line's record: how long the ones let in waited (s), the wait over the first wait each was told, and how many
/// were let in more than a second after one that came into line more than a second after them (none of them dropped)
fn line_summary(waits: &[Wait]) -> Value {
    let mut secs: Vec<f64> = waits.iter().map(|w| (w.went_in - w.told) / 1000.0).collect();
    let mut ratio: Vec<f64> =
        waits.iter().filter_map(|w| w.eta.filter(|e| *e > 0.0).map(|e| (w.went_in - w.told) / 1000.0 / e)).collect();
    let mut clean: Vec<&Wait> = waits.iter().filter(|w| !w.dropped).collect();
    clean.sort_by(|a, b| a.told.total_cmp(&b.told));
    let (mut overtaken, mut j, mut latest) = (0, 0, f64::NEG_INFINITY);
    for b in &clean {
        while j < clean.len() && clean[j].told < b.told - 1000.0 {
            latest = latest.max(clean[j].went_in);
            j += 1;
        }
        if latest > b.went_in + 1000.0 {
            overtaken += 1;
        }
    }
    serde_json::json!({
        "letIn": waits.len(),
        "waitS": { "p50": pct_of(&mut secs, 50.0), "p90": pct_of(&mut secs, 90.0), "p99": pct_of(&mut secs, 99.0), "max": pct_of(&mut secs, 100.0) },
        "waitOverToldEta": { "n": ratio.len(), "p10": pct_of(&mut ratio, 10.0), "p50": pct_of(&mut ratio, 50.0), "p90": pct_of(&mut ratio, 90.0) },
        "overtaken": overtaken,
    })
}

fn main() {
    raise_nofile();
    let a = Arc::new(args());
    let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(a.threads).enable_all().build().expect("runtime");
    rt.block_on(async move {
        let sh = Arc::new(Shared {
            t0: Instant::now(),
            stop: AtomicBool::new(false),
            welcomed: AtomicU64::new(0),
            refused: AtomicU64::new(0),
            closed: AtomicU64::new(0),
            failed: AtomicU64::new(0),
            on: AtomicI64::new(0),
            waiting: AtomicI64::new(0),
            lives: AtomicU64::new(0),
            left: AtomicU64::new(0),
            msgs_out: Counter::new(),
            msgs_in: Counter::new(),
            bytes_out: Counter::new(),
            bytes_in: Counter::new(),
            compact_in: Counter::new(),
            deltas_in: Counter::new(),
            fpred_in: Counter::new(),
            deliveries: Counter::new(),
            snaps_out: Counter::new(),
            dgrams_in: Counter::new(),
            snaps_in: Counter::new(),
            checked: Counter::new(),
            bad: Counter::new(),
            bad_log: Mutex::new(Vec::new()),
            peers: (0..a.n).map(|_| AtomicU32::new(0)).collect(),
            window: Mutex::new(Hists::new()),
            total: Mutex::new(Hists::new()),
            waits: Mutex::new(Vec::new()),
            rejoined_same: AtomicU64::new(0),
            rejoined_lost: AtomicU64::new(0),
            blip: Default::default(),
            blip_ms: Mutex::new(Vec::new()),
        });
        let mut tasks = Vec::new();
        for i in 0..a.n {
            let (a2, sh2) = (a.clone(), sh.clone());
            tasks.push(tokio::spawn(async move {
                sleep(Duration::from_millis(a2.stagger_ms * i as u64)).await;
                run_bot(i, a2, sh2).await
            }));
        }
        let mut samples = Vec::new();
        let mut last = [0u64; 9];
        let mut last_t = Instant::now();
        let ramp = Duration::from_millis(a.stagger_ms * a.n as u64);
        let end = Instant::now() + ramp + Duration::from_secs(a.seconds);
        let mut steady = Hists::new();
        let (mut steady_dl, mut steady_in, mut steady_bytes, mut steady_player_s) = (0u64, 0u64, 0u64, 0f64);
        while Instant::now() < end {
            sleep(Duration::from_secs(5)).await;
            let dt = last_t.elapsed().as_secs_f64();
            last_t = Instant::now();
            let now = [
                sh.msgs_out.get(),
                sh.msgs_in.get(),
                sh.deliveries.get(),
                sh.dgrams_in.get(),
                sh.snaps_out.get(),
                sh.snaps_in.get(),
                sh.bytes_out.get(),
                sh.bytes_in.get(),
                sh.deltas_in.get(),
            ];
            let w = std::mem::replace(&mut *sh.window.lock(), Hists::new());
            let on = sh.on.load(Ordering::Relaxed).max(0);
            let linked: Vec<u32> = sh.peers.iter().map(|p| p.load(Ordering::Relaxed)).filter(|p| *p > 0).collect();
            let peers_mean = if linked.is_empty() { 0.0 } else { linked.iter().sum::<u32>() as f64 / linked.len() as f64 };
            let snaps_out = (now[4] - last[4]) as f64;
            let snaps_in = (now[5] - last[5]) as f64;
            let bytes_in = (now[7] - last[7]) as f64 / dt;
            let in_steady = last_t.duration_since(sh.t0) > ramp + Duration::from_secs(10);
            if in_steady {
                for (x, y) in steady.lat.iter_mut().zip(&w.lat) {
                    *x += y;
                }
                for (x, y) in steady.ping.iter_mut().zip(&w.ping) {
                    *x += y;
                }
                steady_dl += now[4] - last[4];
                steady_in += now[5] - last[5];
                steady_bytes += now[7] - last[7];
                steady_player_s += on as f64 * dt;
            }
            let s = serde_json::json!({
                "tag": a.tag,
                "t": (last_t.duration_since(sh.t0).as_secs_f64() * 10.0).round() / 10.0,
                "players": on, "waiting": sh.waiting.load(Ordering::Relaxed),
                "linked": linked.len(), "peersMean": (peers_mean * 10.0).round() / 10.0,
                "msgsOutPerS": ((now[0] - last[0]) as f64 / dt).round(), "msgsInPerS": ((now[1] - last[1]) as f64 / dt).round(),
                "kbOutPerS": ((now[6] - last[6]) as f64 / dt / 1000.0).round(), "kbInPerS": (bytes_in / 1000.0).round(),
                "bytesInPerPlayerS": if on > 0 { (bytes_in / on as f64).round() } else { 0.0 },
                "deliveriesPerS": ((now[2] - last[2]) as f64 / dt).round(), "dgramsInPerS": ((now[3] - last[3]) as f64 / dt).round(),
                "deltaPct": if now[3] > last[3] { (((now[8] - last[8]) as f64 / (now[3] - last[3]) as f64) * 100.0).round() } else { 0.0 },
                "snapLossPct": if snaps_out > 0.0 { (((snaps_out - snaps_in) / snaps_out * 1000.0).round() / 10.0).max(0.0) } else { 0.0 },
                "latencyMs": { "p50": pct(&w.lat, 50.0), "p90": pct(&w.lat, 90.0), "p99": pct(&w.lat, 99.0), "max": pct(&w.lat, 100.0) },
                "pingMs": { "p50": pct(&w.ping, 50.0), "p99": pct(&w.ping, 99.0), "max": pct(&w.ping, 100.0) },
                "refused": sh.refused.load(Ordering::Relaxed), "failed": sh.failed.load(Ordering::Relaxed), "closed": sh.closed.load(Ordering::Relaxed),
                "lives": sh.lives.load(Ordering::Relaxed), "bad": sh.bad.get(),
            });
            println!("{s}");
            samples.push(s);
            last = now;
        }
        sh.stop.store(true, Ordering::Relaxed);
        sleep(Duration::from_millis(500)).await;
        let t = sh.total.lock();
        let back_ms = {
            let mut v = sh.blip_ms.lock().clone();
            serde_json::json!({ "p50": pct_of(&mut v, 50.0), "p90": pct_of(&mut v, 90.0), "max": pct_of(&mut v, 100.0) })
        };
        let summary = serde_json::json!({
            "tag": a.tag, "n": a.n, "mode": if a.mode == Mode::Roam { "roam" } else { "idle" }, "rate": a.rate, "spread": a.spread,
            "seconds": a.seconds, "compact": a.compact, "delta": a.delta, "fpred": a.fpred, "queue": a.queue, "churn": a.churn,
            "welcomed": sh.welcomed.load(Ordering::Relaxed), "refused": sh.refused.load(Ordering::Relaxed),
            "failed": sh.failed.load(Ordering::Relaxed), "closed": sh.closed.load(Ordering::Relaxed),
            "lives": sh.lives.load(Ordering::Relaxed), "left": sh.left.load(Ordering::Relaxed),
            "snapshots": { "checked": sh.checked.get(), "bad": sh.bad.get(), "compactBundles": sh.compact_in.get(), "deltas": sh.deltas_in.get(), "byFields": sh.fpred_in.get(), "badLog": *sh.bad_log.lock() },
            "steady": {
                "snapLossPct": if steady_dl > 0 { ((steady_dl as f64 - steady_in as f64) / steady_dl as f64 * 1000.0).round() / 10.0 } else { 0.0 },
                "bytesInPerPlayerS": if steady_player_s > 0.0 { (steady_bytes as f64 / steady_player_s).round() } else { 0.0 },
                "latencyMs": { "p50": pct(&steady.lat, 50.0), "p90": pct(&steady.lat, 90.0), "p99": pct(&steady.lat, 99.0), "p999": pct(&steady.lat, 99.9), "max": pct(&steady.lat, 100.0) },
                "pingMs": { "p50": pct(&steady.ping, 50.0), "p90": pct(&steady.ping, 90.0), "p99": pct(&steady.ping, 99.0), "max": pct(&steady.ping, 100.0) },
            },
            "all": {
                "latencyMs": { "p50": pct(&t.lat, 50.0), "p99": pct(&t.lat, 99.0), "max": pct(&t.lat, 100.0) },
                "pingMs": { "p50": pct(&t.ping, 50.0), "p99": pct(&t.ping, 99.0), "max": pct(&t.ping, 100.0) },
            },
            "line": line_summary(&sh.waits.lock()),
            "rejoined": { "samePlace": sh.rejoined_same.load(Ordering::Relaxed), "lostPlace": sh.rejoined_lost.load(Ordering::Relaxed) },
            "blip": {
                "dropped": sh.blip[0].load(Ordering::Relaxed), "sameAddress": sh.blip[1].load(Ordering::Relaxed),
                "newAddress": sh.blip[2].load(Ordering::Relaxed), "throughTheLine": sh.blip[3].load(Ordering::Relaxed),
                "notBack": sh.blip[4].load(Ordering::Relaxed),
                "backMs": back_ms,
            },
        });
        drop(t);
        println!("{summary}");
        if !a.out.is_empty() {
            let doc = serde_json::json!({ "summary": summary, "samples": samples });
            std::fs::write(&a.out, serde_json::to_string_pretty(&doc).unwrap()).expect("--out");
        }
        std::process::exit(0);
    });
}
