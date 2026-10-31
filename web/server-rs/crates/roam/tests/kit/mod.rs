//! What the hub's tests share: a player's datagrams as roam_core.c lays them out, the switch's frame around them, the
//! lists out of the hub's frames, and a hub with a clock of its own.
#![allow(dead_code)]

use std::collections::HashMap;
use std::sync::Arc;

use bytes::Bytes;
use nfsu2_roam::{Hub, PeerEntry, ROAM_HUB_VIP, RoamOptions};
use parking_lot::Mutex;

/// "10.77.0.5" as a number
pub fn ip(s: &str) -> u32 {
    s.split('.').fold(0u32, |a, b| (a << 8) | b.parse::<u32>().unwrap_or(0))
}

/// roam_core's room hash (FNV-1a of the room's name, 0 for none, never 0 for one)
pub fn fnv1a(s: &str) -> u32 {
    if s.is_empty() {
        return 0;
    }
    let mut h: u32 = 0x811C_9DC5;
    for c in s.chars() {
        h ^= c as u32 & 0xff;
        h = h.wrapping_mul(0x0100_0193);
    }
    if h == 0 { 1 } else { h }
}

/// roam_core.c's rc_room_hash of an ASCII name, a Room's or a crew's (the page's players.ts roomHash is the same; both also fold
/// capitals past A to Z and Unicode's blanks, which these tests' names never have): FNV-1a of the name's
/// UTF-8 without its blanks (space, \t to \r), A to Z made small; 0 for a name of blanks only, never 0 for another
/// (`fnv1a` is the same for a name of small letters without blanks)
pub fn room_hash(s: &str) -> u32 {
    let (mut h, mut kept) = (0x811C_9DC5u32, 0);
    for b in s.bytes().filter(|&b| b != b' ' && !(b'\t'..=b'\r').contains(&b)) {
        h ^= b.to_ascii_lowercase() as u32;
        h = h.wrapping_mul(0x0100_0193);
        kept += 1;
    }
    if kept == 0 { 0 } else { h.max(1) }
}

/// a datagram: the header, the payload, the CRC
pub fn dgram(kind: u8, sender: u32, payload: &[u8]) -> Vec<u8> {
    dgram_flags(kind, sender, 0, payload)
}

/// a datagram with these header flags
pub fn dgram_flags(kind: u8, sender: u32, flags: u16, payload: &[u8]) -> Vec<u8> {
    let len = 16 + payload.len() + 4;
    let mut b = vec![0u8; len];
    b[..4].copy_from_slice(b"N2FR");
    b[4] = 1;
    b[5] = kind;
    b[6..8].copy_from_slice(&(len as u16).to_le_bytes());
    b[8..12].copy_from_slice(&sender.to_le_bytes());
    b[12..14].copy_from_slice(&7u16.to_le_bytes());
    b[14..16].copy_from_slice(&flags.to_le_bytes());
    b[16..16 + payload.len()].copy_from_slice(payload);
    let crc = crc32fast::hash(&b[..len - 4]);
    b[len - 4..].copy_from_slice(&crc.to_le_bytes());
    b
}

/// the HELLO flag of a player that reads crews (roam_core.h RC_HF_CREW)
pub const HF_CREW: u16 = 0x0200;

pub struct HelloOpts<'a> {
    pub room: u32,
    pub status: u8,
    pub name: &'a [u8],
    pub seen: &'a [u32],
    pub cfg_rev: u16,
    pub core: u16,
    /// its crews: with them the HELLO has the crew flag and ends with them (a count, the hashes); None, a HELLO of
    /// before
    pub crews: Option<&'a [u32]>,
}

impl Default for HelloOpts<'_> {
    fn default() -> Self {
        HelloOpts { room: 0, status: 2, name: b"p", seen: &[], cfg_rev: 3, core: 9, crews: None }
    }
}

pub fn hello_with(sender: u32, o: &HelloOpts) -> Vec<u8> {
    let nl = o.name.len();
    let mut p = vec![0u8; 13 + nl + 1 + 4 * o.seen.len()];
    p[0..2].copy_from_slice(&o.core.to_le_bytes());
    p[2] = 1;
    p[3] = o.status;
    p[4..6].copy_from_slice(&47210u16.to_le_bytes());
    p[6..8].copy_from_slice(&o.cfg_rev.to_le_bytes());
    p[8..12].copy_from_slice(&o.room.to_le_bytes());
    p[12] = nl as u8;
    p[13..13 + nl].copy_from_slice(o.name);
    p[13 + nl] = o.seen.len() as u8;
    for (i, s) in o.seen.iter().enumerate() {
        let at = 14 + nl + 4 * i;
        p[at..at + 4].copy_from_slice(&s.to_le_bytes());
    }
    let Some(crews) = o.crews else { return dgram(1, sender, &p) };
    p.push(crews.len() as u8);
    for c in crews {
        p.extend_from_slice(&c.to_le_bytes());
    }
    dgram_flags(1, sender, HF_CREW, &p)
}

pub fn hello(sender: u32) -> Vec<u8> {
    hello_with(sender, &HelloOpts::default())
}

/// a SNAP of `n` states (the newest one's position first)
pub fn snap_n(sender: u32, pos: [f64; 3], status: u8, n: u8) -> Vec<u8> {
    let mut p = vec![0u8; 6 + 52 * n as usize];
    p[0] = status;
    p[4] = n;
    p[10..14].copy_from_slice(&(pos[0] as f32).to_le_bytes());
    p[14..18].copy_from_slice(&(pos[1] as f32).to_le_bytes());
    p[18..22].copy_from_slice(&(pos[2] as f32).to_le_bytes());
    // q.w (a valid quaternion)
    p[6 + 34..6 + 36].copy_from_slice(&32767i16.to_le_bytes());
    dgram(3, sender, &p)
}

pub fn snap(sender: u32, pos: [f64; 3]) -> Vec<u8> {
    snap_n(sender, pos, 2, 1)
}

/// a SNAP of one state with a velocity and a heading (radians, 0 = +x, a turn about z)
pub fn snap_moving(sender: u32, pos: [f64; 3], v: [f64; 3], heading: f64, status: u8) -> Vec<u8> {
    let mut d = snap_n(sender, pos, status, 1);
    let p = &mut d[16..];
    for k in 0..3 {
        p[22 + 4 * k..26 + 4 * k].copy_from_slice(&(v[k] as f32).to_le_bytes());
    }
    let (z, w) = ((heading / 2.0).sin(), (heading / 2.0).cos());
    p[34..36].copy_from_slice(&0i16.to_le_bytes());
    p[38..40].copy_from_slice(&((z * 32767.0).round() as i16).to_le_bytes());
    p[40..42].copy_from_slice(&((w * 32767.0).round() as i16).to_le_bytes());
    let len = d.len();
    let crc = crc32fast::hash(&d[..len - 4]);
    d[len - 4..].copy_from_slice(&crc.to_le_bytes());
    d
}

/// the world map's question (MAPREQ): for max players at most, what changed since its answer base (0: none)
pub fn mapreq_base(sender: u32, req: u16, max: u16, base: u16) -> Vec<u8> {
    dgram(10, sender, &[req.to_le_bytes(), max.to_le_bytes(), base.to_le_bytes()].concat())
}

/// a MAPD entry: the player's key, and where it is placed (x, y, heading, vx, vy), or None: gone
pub type MapdEntry = (u16, Option<(i16, i16, u8, i8, i8)>);
/// a MAPD datagram: (req, base, part, nparts, total, entries)
pub type Mapd = (u16, u16, u8, u8, u16, Vec<MapdEntry>);

/// the MAPD datagram in a frame of the hub's
pub fn mapd_of(f: &[u8]) -> Option<Mapd> {
    let d = &f[16..];
    if d[5] != 17 {
        return None;
    }
    let (mut o, mut e) = (25, Vec::new());
    for _ in 0..d[24] {
        let key = u16::from_le_bytes([d[o], d[o + 1]]);
        if key & 0x8000 != 0 {
            e.push((key & 0x7fff, None));
            o += 2;
        } else {
            let i = |k: usize| i16::from_le_bytes([d[o + k], d[o + k + 1]]);
            e.push((key, Some((i(2), i(4), d[o + 6], d[o + 7] as i8, d[o + 8] as i8))));
            o += 9;
        }
    }
    assert_eq!(o + 4, d.len(), "MAPD entries fill the datagram");
    let u = |k: usize| u16::from_le_bytes([d[k], d[k + 1]]);
    Some((u(16), u(18), d[20], d[21], u(22), e))
}

pub fn bye(sender: u32) -> Vec<u8> {
    dgram(8, sender, &[0])
}

/// the switch's frame around a datagram (kind 1: [12..14] source port, [14..16] destination port)
pub fn frame_ports(from: u32, d: &[u8], sport: u16, dport: u16) -> Vec<u8> {
    let mut f = vec![0u8; 16 + d.len()];
    f[0] = 1;
    f[4..8].copy_from_slice(&from.to_be_bytes());
    f[8..12].copy_from_slice(&ROAM_HUB_VIP.to_be_bytes());
    f[12..14].copy_from_slice(&sport.to_be_bytes());
    f[14..16].copy_from_slice(&dport.to_be_bytes());
    f[16..].copy_from_slice(d);
    f
}

pub fn frame(from: u32, d: &[u8]) -> Vec<u8> {
    frame_ports(from, d, 47210, 47210)
}

/// the PEERS list in a frame of the hub's
pub fn peers_of(f: &[u8]) -> Vec<PeerEntry> {
    let d = &f[16..];
    (0..d[16] as usize)
        .map(|i| {
            let o = 17 + 10 * i;
            PeerEntry {
                vip: u32::from_be_bytes(d[o..o + 4].try_into().unwrap()),
                port: u16::from_le_bytes(d[o + 4..o + 6].try_into().unwrap()),
                id: u32::from_le_bytes(d[o + 6..o + 10].try_into().unwrap()),
            }
        })
        .collect()
}

/// the crew marks a PEERS list in a frame of the hub's ends in (roam_core.h RC_HF_CREW on a PEERS): the receiver's
/// crews and a mark an entry (bit c: in its crew c too); None for a list without them
pub fn crew_marks(f: &[u8]) -> Option<(Vec<u32>, Vec<u8>)> {
    let d = &f[16..];
    if u16::from_le_bytes([d[14], d[15]]) & HF_CREW == 0 {
        return None;
    }
    let n = d[16] as usize;
    let o = 17 + 10 * n;
    let k = d[o] as usize;
    let mine = (0..k).map(|c| u32::from_le_bytes(d[o + 1 + 4 * c..o + 5 + 4 * c].try_into().unwrap())).collect();
    Some((mine, d[o + 1 + 4 * k..o + 1 + 4 * k + n].to_vec()))
}

/// the PEERS lists out of the hub's frames to the browsers, by `vip:port`
pub fn lists(sent: &[(u32, Bytes)]) -> HashMap<String, Vec<PeerEntry>> {
    sent.iter().map(|(vip, f)| (format!("{vip}:{}", u16::from_be_bytes([f[14], f[15]])), peers_of(f))).collect()
}

/// a hub with a clock of its own (1,000,000 ms at the start), an idle server unless a test says otherwise (the tests
/// tick back to back: a real measure would look busy), every list sent
pub struct Kit {
    pub hub: Hub,
    pub sent: Arc<Mutex<Vec<(u32, Bytes)>>>,
    pub t: Arc<Mutex<f64>>,
    pub busy: Arc<Mutex<f64>>,
    pub lines: Arc<Mutex<Vec<String>>>,
}

impl Kit {
    pub fn new(o: RoamOptions) -> Kit {
        let sent = Arc::new(Mutex::new(Vec::new()));
        let t = Arc::new(Mutex::new(1_000_000.0));
        let busy = Arc::new(Mutex::new(0.0));
        let lines = Arc::new(Mutex::new(Vec::new()));
        let (s, tt, b, l) = (sent.clone(), t.clone(), busy.clone(), lines.clone());
        let hub = Hub::new(
            o,
            Box::new(move || *tt.lock()),
            Box::new(move || *b.lock()),
            Box::new(move |line: &str| l.lock().push(line.to_string())),
            Box::new(move |vip, f| {
                s.lock().push((vip, f));
                true
            }),
            0xB0B,
        );
        Kit { hub, sent, t, busy, lines }
    }
    pub fn step(&self, ms: f64) {
        *self.t.lock() += ms;
    }
    pub fn lists(&self) -> HashMap<String, Vec<PeerEntry>> {
        lists(&self.sent.lock())
    }
    pub fn clear(&self) {
        self.sent.lock().clear();
    }
    /// a datagram from browser `vip` (port 47210)
    pub fn from(&mut self, vip: u32, d: &[u8]) {
        self.hub.frame(vip, &frame(vip, d));
    }
    pub fn info(&self, k: &str) -> f64 {
        self.hub.info()[k].as_f64().unwrap_or(f64::NAN)
    }
}

/// the links of a room: every pair weighed (a room of at most exactRoom players), or the grid's nearest (a larger one);
/// without view points (the tests' cars point along x: theirs are roam_ahead.rs's), so by the distances (and speeds)
pub fn ways() -> [(&'static str, RoamOptions); 2] {
    let o = RoamOptions { view_m: 1e-9, ..RoamOptions::default() };
    [("every pair", o.clone()), ("the grid", RoamOptions { exact_room: 1, ..o })]
}

/// browser i's address: 10.77.0.(10 + i)
#[allow(non_snake_case)]
pub fn V(i: u32) -> u32 {
    ip(&format!("10.77.0.{}", 10 + i))
}
