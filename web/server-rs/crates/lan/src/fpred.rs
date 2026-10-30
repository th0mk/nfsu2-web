//! Free roam's car states by their fields (a page whose hello said "fpred":1 besides "delta":1): a friend's SNAP of one
//! state (runtime/roam_core.c rc_encode, put_state, put_statex) goes to it as the fields that differ from what the page
//! can work out itself, each as a small number.  A byte-level delta (compact.rs `Refs`) sends every byte a float's
//! last bits touched; here
//! - the position goes in millimetres, the velocity in centimetres a second and the time in steps of 256 µs: the page
//!   writes back the floats and microseconds those make, so the game gets the state rounded that far (at most 0.5 mm,
//!   0.5 cm/s and 128 µs off, about 4 mm along the road at 30 m/s), with a CRC that checks out;
//! - the time is taken to go on as it went (the last interval again), the sequence number too, and the position by the
//!   last one's velocity over the time between: what is left of them is mostly nothing or a few millimetres;
//! - every other field is taken to be what it was, and goes as its difference when it is not.
//!
//! On real drives (roam-pair, 4 games, 2026-10-24) a car state took 36 bytes an entry as a byte delta, about 20 so.
//!
//! An entry: [0] `F_FPRED`, [1..2] the source's low 16 bits, [3] the length of what follows (at most 255: a longer one
//! goes as the byte delta instead); then a bitmap of the fields that differ (LEB128, 7 a byte, field 0 in bit 0, the
//! groups of zeros at the end left out), then each of those as a zigzag LEB128 varint of the difference (`diff`).  The
//! fields are in `NF` order, the often-changing first.  Both ends keep each source's last state (as the page restores
//! it: the rounded one), and the one before for the time and the sequence; a source with none goes against zeros.  Past
//! `REFS_MAX` sources, a new one clears them all first, on both ends.  Anything else of the datagram that a state of
//! this layout cannot carry (more states than one, a field of a newer core) goes as before, and leaves this state alone.
//!
//! The arithmetic is integers only, the same on the page (web/src/net/proto.ts `FpredState`): the rounding of the
//! position, the velocity and the time is done here, once; the page and this end only turn integers into floats (one
//! IEEE division and a conversion to f32, the same everywhere).

use std::collections::HashMap;
use std::hash::BuildHasherDefault;

use crate::compact::{KeyHash, REFS_MAX};

/// the entry's flags byte: the kind 1 with F_ROAM and F_WHOLE, which together go with nothing else (compact.rs)
pub const F_FPRED: u8 = 0x61;
/// the fields: the time and sequence number first (a standing car's only changes, mostly), then what a moving one
/// changes
pub const NF: usize = 53;
const T: usize = 0; // steps of 256 µs, 24 bits
const SEQ: usize = 1;
const P: usize = 2; // x, y, z: millimetres
const V: usize = 5; // centimetres a second
const Q: usize = 8; // 4, i16
const W: usize = 12; // 3, i16
const SPEED: usize = 15;
const RPM: usize = 16;
const STEER: usize = 17;
const WZ: usize = 18; // 4, i16
const ENG: usize = 22;
const EACC: usize = 23;
const EDCL: usize = 24;
const THR: usize = 25;
const BRK: usize = 26;
const GEAR: usize = 27;
const BITS: usize = 28;
const NOS: usize = 29;
const HYD: usize = 30;
const THUMPS: usize = 31;
const EBASE: usize = 32;
const SLIP: usize = 33; // 12 bytes: each wheel's two slips and its surface
const XLEN: usize = 45;
const CLOCK: usize = 46;
const STATUS: usize = 47;
const EPOCH: usize = 48;
const CFG_REV: usize = 49;
const N: usize = 50;
const FLAGS: usize = 51;
const SENDER: usize = 52;
/// runtime/roam_core.h RC_HF_STATEX: the header's flag for a state's extension
const HF_STATEX: i64 = 0x0040;

/// a field's width when its difference wraps (bits), 0 for a signed one that does not
const fn width(i: usize) -> u32 {
    match i {
        RPM | ENG | SEQ | CFG_REV | FLAGS => 16,
        T => 24,
        SENDER => 32,
        EACC | EDCL | THR | BRK | GEAR | BITS | NOS | HYD | THUMPS | EBASE | XLEN | CLOCK | STATUS | EPOCH | N => 8,
        i if i >= SLIP && i < SLIP + 12 => 8,
        _ => 0,
    }
}

/// a car state's fields (`NF`), as integers
pub type Fields = [i64; NF];

/// what an extension of `x` bytes holds past the wheels' heights and the nitrous, as roam_core.c get_statex reads it:
/// the hydraulics, the wheels' slips, the engine; None when a newer core's fields follow those (or it is too short)
fn layout(x: usize) -> Option<(bool, bool, bool)> {
    let (hyd, slip) = (x >= 11, x >= 23);
    let base = 9 + if hyd { 2 } else { 0 } + if slip { 12 } else { 0 };
    let eng = x >= base + 5;
    (x >= 9 && base + if eng { 5 } else { 0 } == x).then_some((hyd, slip, eng))
}

fn rd16(b: &[u8], o: usize) -> i64 {
    u16::from_le_bytes([b[o], b[o + 1]]) as i64
}
fn rds16(b: &[u8], o: usize) -> i64 {
    i16::from_le_bytes([b[o], b[o + 1]]) as i64
}
fn rd32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}
fn rdf(b: &[u8], o: usize) -> f32 {
    f32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

/// The fields of a SNAP's body (the datagram from its sender's id on, without its CRC: what a compact entry carries),
/// rounded as they go; None when it is not a SNAP of one state of this layout, or a float is not a finite one this
/// can carry.
pub fn fields(body: &[u8]) -> Option<Fields> {
    if body.len() < 66 || body[12] != 1 {
        return None;
    }
    let mut v = [0i64; NF];
    v[SENDER] = rd32(body, 0) as i64;
    v[SEQ] = rd16(body, 4);
    v[FLAGS] = rd16(body, 6);
    v[STATUS] = body[8] as i64;
    v[EPOCH] = body[9] as i64;
    v[CFG_REV] = rd16(body, 10);
    v[N] = 1;
    v[CLOCK] = body[13] as i64;
    v[T] = ((rd32(body, 14) as i64 + 128) >> 8) & 0xFF_FFFF;
    for i in 0..3 {
        let (p, s) = (rdf(body, 18 + 4 * i), rdf(body, 30 + 4 * i));
        if !p.is_finite() || p.abs() > 1e6 || !s.is_finite() || s.abs() > 1e6 {
            return None;
        }
        v[P + i] = (p as f64 * 1000.0).round() as i64;
        v[V + i] = (s as f64 * 100.0).round() as i64;
    }
    for i in 0..4 {
        v[Q + i] = rds16(body, 42 + 2 * i);
    }
    for i in 0..3 {
        v[W + i] = rds16(body, 50 + 2 * i);
    }
    v[STEER] = rds16(body, 56);
    v[THR] = body[58] as i64;
    v[BRK] = body[59] as i64;
    v[RPM] = rd16(body, 60);
    v[SPEED] = rds16(body, 62);
    v[GEAR] = body[64] as i64;
    v[BITS] = body[65] as i64;
    let mut end = 66;
    if v[FLAGS] & HF_STATEX != 0 {
        if body.len() < 67 {
            return None;
        }
        let x = body[66] as usize;
        v[XLEN] = x as i64;
        end = 67 + x;
        if x > 0 {
            let (hyd, slip, eng) = layout(x)?;
            if body.len() < end {
                return None;
            }
            for i in 0..4 {
                v[WZ + i] = rds16(body, 67 + 2 * i);
            }
            v[NOS] = body[75] as i64;
            let mut k = 76;
            if hyd {
                v[HYD] = body[76] as i64;
                v[THUMPS] = body[77] as i64;
                k = 78;
            }
            if slip {
                for i in 0..12 {
                    v[SLIP + i] = body[78 + i] as i64;
                }
                k = 90;
            }
            if eng {
                v[ENG] = rd16(body, k);
                v[EACC] = body[k + 2] as i64;
                v[EDCL] = body[k + 3] as i64;
                v[EBASE] = body[k + 4] as i64;
            }
        }
    }
    (body.len() == end).then_some(v)
}

/// The body a state's fields make (the page does the same, proto.ts): its floats and time as the page writes them back.
/// False when the fields make no state of this layout (nothing is written then).
pub fn write(v: &Fields, out: &mut Vec<u8>) -> bool {
    let x = v[XLEN] as usize;
    let groups = if v[FLAGS] & HF_STATEX != 0 && x > 0 {
        match layout(x) {
            Some(g) => Some(g),
            None => return false,
        }
    } else {
        None
    };
    if v[N] != 1 || !(0..256).contains(&v[XLEN]) {
        return false;
    }
    out.extend_from_slice(&(v[SENDER] as u32).to_le_bytes());
    out.extend_from_slice(&(v[SEQ] as u16).to_le_bytes());
    out.extend_from_slice(&(v[FLAGS] as u16).to_le_bytes());
    out.extend_from_slice(&[v[STATUS] as u8, v[EPOCH] as u8]);
    out.extend_from_slice(&(v[CFG_REV] as u16).to_le_bytes());
    out.extend_from_slice(&[1, v[CLOCK] as u8]);
    out.extend_from_slice(&((v[T] as u32) << 8).to_le_bytes());
    for i in 0..3 {
        out.extend_from_slice(&((v[P + i] as f64 / 1000.0) as f32).to_le_bytes());
    }
    for i in 0..3 {
        out.extend_from_slice(&((v[V + i] as f64 / 100.0) as f32).to_le_bytes());
    }
    for i in Q..Q + 4 {
        out.extend_from_slice(&(v[i] as i16).to_le_bytes());
    }
    for i in W..W + 3 {
        out.extend_from_slice(&(v[i] as i16).to_le_bytes());
    }
    out.extend_from_slice(&(v[STEER] as i16).to_le_bytes());
    out.extend_from_slice(&[v[THR] as u8, v[BRK] as u8]);
    out.extend_from_slice(&(v[RPM] as u16).to_le_bytes());
    out.extend_from_slice(&(v[SPEED] as i16).to_le_bytes());
    out.extend_from_slice(&[v[GEAR] as u8, v[BITS] as u8]);
    if v[FLAGS] & HF_STATEX != 0 {
        out.push(x as u8);
        if let Some((hyd, slip, eng)) = groups {
            for i in WZ..WZ + 4 {
                out.extend_from_slice(&(v[i] as i16).to_le_bytes());
            }
            out.push(v[NOS] as u8);
            if hyd {
                out.extend_from_slice(&[v[HYD] as u8, v[THUMPS] as u8]);
            }
            if slip {
                out.extend((SLIP..SLIP + 12).map(|i| v[i] as u8));
            }
            if eng {
                out.extend_from_slice(&(v[ENG] as u16).to_le_bytes());
                out.extend_from_slice(&[v[EACC] as u8, v[EDCL] as u8, v[EBASE] as u8]);
            }
        }
    }
    true
}

/// a SNAP's body as the game gets it through this (rounded; None: it would go as before, as it is)
pub fn rounded(body: &[u8]) -> Option<Vec<u8>> {
    let v = fields(body)?;
    let mut out = Vec::with_capacity(body.len());
    write(&v, &mut out).then_some(out)
}

/// what both ends keep of a source: its last state, and the one before's time and sequence number (`two`: there was one)
#[derive(Clone)]
struct Last {
    v: Fields,
    t2: i64,
    seq2: i64,
    two: bool,
}

/// the value field i is taken to have: its last, the time and sequence number as they went, the position carried on by
/// the last velocity for the time since (`t`: this state's time, when known)
fn predict(last: Option<&Last>, t: Option<i64>) -> Fields {
    let Some(l) = last else { return [0; NF] };
    let mut w = l.v;
    if l.two {
        w[T] = (2 * l.v[T] - l.t2) & 0xFF_FFFF;
        w[SEQ] = (2 * l.v[SEQ] - l.seq2) & 0xFFFF;
    }
    if let Some(t) = t {
        let dt = signed(t - l.v[T], 24);
        // at most about a second: anything longer is a new start, not a drive
        if (0..=4096).contains(&dt) {
            for i in 0..3 {
                // mm = cm/s x 10 x dt x 256 µs, rounded: the velocity times dt x 16 / 6250
                w[P + i] = l.v[P + i] + (l.v[V + i] * dt * 16 + 3125).div_euclid(6250);
            }
        }
    }
    w
}

/// a difference of `bits` bits, signed (0: as it is): its low bits, the top one the sign
fn signed(d: i64, bits: u32) -> i64 {
    if bits == 0 {
        return d;
    }
    (d << (64 - bits)) >> (64 - bits)
}

fn diff(i: usize, cur: i64, want: i64) -> i64 {
    signed(cur - want, width(i))
}

fn apply(i: usize, want: i64, d: i64) -> i64 {
    match width(i) {
        0 => want + d,
        b => (want + d) & ((1 << b) - 1),
    }
}

fn put_varint(buf: &mut Vec<u8>, mut u: u64) {
    while u >= 0x80 {
        buf.push(u as u8 | 0x80);
        u >>= 7;
    }
    buf.push(u as u8);
}

fn get_varint(b: &[u8], at: &mut usize) -> Option<u64> {
    let mut u = 0u64;
    for k in 0..10 {
        let x = *b.get(*at)?;
        *at += 1;
        u |= ((x & 0x7F) as u64) << (7 * k);
        if x & 0x80 == 0 {
            return Some(u);
        }
    }
    None
}

/// One end's last states by source (the switch's writer for its page, or a decoder of the page's: `Decoder`).
#[derive(Default)]
pub struct Fpred {
    last: HashMap<u16, Last, BuildHasherDefault<KeyHash>>,
}

impl Fpred {
    fn keep(&mut self, src: u16, v: &Fields) {
        if let Some(l) = self.last.get_mut(&src) {
            (l.t2, l.seq2, l.two) = (l.v[T], l.v[SEQ], true);
            l.v = *v;
            return;
        }
        if self.last.len() >= REFS_MAX {
            self.last.clear();
        }
        self.last.insert(src, Last { v: *v, t2: 0, seq2: 0, two: false });
    }

    /// The entry of a state from `src` into `buf`; false when it would be longer than 255 (nothing written, nothing
    /// kept: it goes as before)
    pub fn encode(&mut self, buf: &mut Vec<u8>, src: u16, cur: &Fields) -> bool {
        let want = predict(self.last.get(&src), Some(cur[T]));
        let start = buf.len();
        buf.push(F_FPRED);
        buf.extend_from_slice(&src.to_be_bytes());
        buf.push(0);
        let mut d = [0i64; NF];
        let mut mask = 0u64;
        for i in 0..NF {
            d[i] = diff(i, cur[i], want[i]);
            if d[i] != 0 {
                mask |= 1 << i;
            }
        }
        put_varint(buf, mask);
        for &x in d.iter().filter(|&&x| x != 0) {
            put_varint(buf, ((x << 1) ^ (x >> 63)) as u64);
        }
        let len = buf.len() - start - 4;
        if len > 255 {
            buf.truncate(start);
            return false;
        }
        buf[start + 3] = len as u8;
        self.keep(src, cur);
        true
    }

    /// An entry's state from `src` (`e`: what follows its 4-byte head) as its body (the datagram from its sender's id on,
    /// without the CRC); None when it is broken.  What the page does (proto.ts `FpredState.take`).
    pub fn decode(&mut self, src: u16, e: &[u8]) -> Option<Vec<u8>> {
        let mut at = 0;
        let mask = get_varint(e, &mut at)?;
        if mask >> NF != 0 {
            return None;
        }
        let mut d = [0i64; NF];
        for (i, x) in d.iter_mut().enumerate() {
            if mask >> i & 1 == 1 {
                let u = get_varint(e, &mut at)?;
                *x = (u >> 1) as i64 ^ -((u & 1) as i64);
            }
        }
        if at != e.len() {
            return None;
        }
        let last = self.last.get(&src);
        let t = apply(T, predict(last, None)[T], d[T]);
        let want = predict(last, Some(t));
        let mut cur = [0i64; NF];
        for i in 0..NF {
            cur[i] = apply(i, want[i], d[i]);
        }
        let mut body = Vec::with_capacity(96);
        if !write(&cur, &mut body) {
            return None;
        }
        self.keep(src, &cur);
        Some(body)
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// a SNAP's body (the datagram from its sender's id on, without the CRC) as roam_core.c rc_encode makes it: one
    /// state, with an extension of `x` bytes when `x` is Some (its fields from `k`, so that each state differs)
    pub(crate) fn body(sender: u32, seq: u16, t: u32, p: [f32; 3], v: [f32; 3], k: i32, x: Option<u8>) -> Vec<u8> {
        let mut b = Vec::new();
        b.extend_from_slice(&sender.to_le_bytes());
        b.extend_from_slice(&seq.to_le_bytes());
        b.extend_from_slice(&(if x.is_some() { 0x0040u16 } else { 0 } | 0x0020).to_le_bytes());
        b.extend_from_slice(&[2, 7]);
        b.extend_from_slice(&0x0102u16.to_le_bytes());
        b.extend_from_slice(&[1, 3]);
        b.extend_from_slice(&t.to_le_bytes());
        for f in p.iter().chain(v.iter()) {
            b.extend_from_slice(&f.to_le_bytes());
        }
        for q in [100 + k, -200, 30000 - k, 5] {
            b.extend_from_slice(&(q as i16).to_le_bytes());
        }
        for w in [k * 3, -k, 7] {
            b.extend_from_slice(&(w as i16).to_le_bytes());
        }
        b.extend_from_slice(&((k * 101) as i16).to_le_bytes());
        b.extend_from_slice(&[200, (k % 3) as u8]);
        b.extend_from_slice(&(3000 + 7 * k as u16).to_le_bytes());
        b.extend_from_slice(&((1500 + k) as i16).to_le_bytes());
        b.extend_from_slice(&[4, 0x10]);
        if let Some(x) = x {
            b.push(x);
            if x > 0 {
                for i in 0..4 {
                    b.extend_from_slice(&((-300 + k + i) as i16).to_le_bytes());
                }
                b.push(0);
                let (hyd, slip, eng) = layout(x as usize).expect("a layout");
                if hyd {
                    b.extend_from_slice(&[1, 2]);
                }
                if slip {
                    b.extend((0..12).map(|i| (i * 9 + k) as u8));
                }
                if eng {
                    b.extend_from_slice(&(4000 + k as u16).to_le_bytes());
                    b.extend_from_slice(&[10, 20, 30]);
                }
            }
        }
        b
    }

    /// through an encoder and a decoder (the switch's writer and the page): what the page writes is the rounded body
    fn through(enc: &mut Fpred, dec: &mut Fpred, src: u16, b: &[u8]) -> usize {
        let v = fields(b).expect("a state this carries");
        let mut buf = Vec::new();
        assert!(enc.encode(&mut buf, src, &v));
        assert_eq!(buf[0], F_FPRED);
        assert_eq!(u16::from_be_bytes([buf[1], buf[2]]), src);
        assert_eq!(buf[3] as usize, buf.len() - 4);
        let got = dec.decode(src, &buf[4..]).expect("it decodes");
        assert_eq!(got, rounded(b).unwrap(), "the rounded body");
        buf.len()
    }

    #[test]
    fn every_extension_comes_back_rounded() {
        for x in [None, Some(0), Some(9), Some(11), Some(16), Some(23), Some(28)] {
            let (mut enc, mut dec) = (Fpred::default(), Fpred::default());
            for k in 0..5 {
                let b = body(0xABCD_0001, 10 + k as u16, 1_000_000 + 50_000 * k as u32, [1695.123, -884.5, 24.25], [12.5, -3.25, 0.01], k, x);
                through(&mut enc, &mut dec, 0x0102, &b);
            }
        }
    }

    #[test]
    fn the_rounding_is_that_far_and_no_farther() {
        let b = body(1, 1, 123_456_789, [1695.1234, -884.5006, 24.2496], [12.345, -3.2549, 0.004], 0, Some(16));
        let r = rounded(&b).unwrap();
        for i in 0..3 {
            let (a, c) = (rdf(&b, 18 + 4 * i) as f64, rdf(&r, 18 + 4 * i) as f64);
            assert!((a - c).abs() <= 0.0005 + 1e-4, "position {i}: {a} {c}");
            let (a, c) = (rdf(&b, 30 + 4 * i) as f64, rdf(&r, 30 + 4 * i) as f64);
            assert!((a - c).abs() <= 0.005 + 1e-6, "velocity {i}: {a} {c}");
        }
        assert!((rd32(&b, 14) as i64 - rd32(&r, 14) as i64).abs() <= 128);
        assert_eq!(b[42..], r[42..], "the rest as it was");
        assert_eq!(b[..14], r[..14]);
    }

    // a car driving a circle at 20 a second, its frames a little late now and then: each comes back rounded, a moving
    // one in a few bytes, a standing one in fewer
    #[test]
    fn a_drive_is_a_few_bytes_a_state() {
        let (mut enc, mut dec) = (Fpred::default(), Fpred::default());
        let (mut moving, mut standing) = (0, 0);
        let mut t = 4_000_000_000u32; // wraps on the way
        for k in 0..400i32 {
            t = t.wrapping_add(50_000 + if k % 7 == 0 { 16_667 } else { 0 });
            let a = k as f64 * 0.05;
            let (p, v) = if k < 300 {
                ([(1000.0 + 40.0 * a.cos()) as f32, (-500.0 + 40.0 * a.sin()) as f32, 24.0], [(-40.0 * a.sin()) as f32, (40.0 * a.cos()) as f32, 0.0])
            } else {
                ([1040.0, -500.0, 24.0], [0.0, 0.0, 0.0])
            };
            let b = body(7, k as u16, t, p, v, if k < 300 { k } else { 300 }, Some(16));
            let n = through(&mut enc, &mut dec, 0x0305, &b);
            if k > 2 && k < 300 {
                moving += n;
            } else if k > 302 {
                standing += n;
            }
        }
        let (m, s) = (moving as f64 / 297.0, standing as f64 / 97.0);
        assert!(m < 30.0 && s < 8.0, "{m:.1} bytes a moving state, {s:.1} a standing one");
    }

    #[test]
    fn what_this_cannot_carry_goes_as_before() {
        let ok = body(1, 1, 1, [1.0; 3], [0.0; 3], 0, Some(16));
        assert!(fields(&ok).is_some());
        let mut three = ok.clone();
        three[12] = 3;
        assert!(fields(&three).is_none(), "more states than one");
        for x in [10u8, 12, 14, 24, 30] {
            let mut b = body(1, 1, 1, [1.0; 3], [0.0; 3], 0, Some(0));
            b[66] = x;
            b.extend(std::iter::repeat_n(0, x as usize));
            assert!(fields(&b).is_none(), "an extension of {x}: a newer core's fields");
        }
        for f in [f32::NAN, f32::INFINITY, 2e6] {
            assert!(fields(&body(1, 1, 1, [f, 0.0, 0.0], [0.0; 3], 0, None)).is_none());
            assert!(fields(&body(1, 1, 1, [0.0; 3], [0.0, f, 0.0], 0, None)).is_none());
        }
        let mut long = ok.clone();
        long.push(0);
        assert!(fields(&long).is_none() && fields(&ok[..ok.len() - 1]).is_none(), "a length that is not the layout's");
        // fields that would take more than 255 bytes: not written, not kept
        let mut enc = Fpred::default();
        let mut big = [0i64; NF];
        for (i, x) in big.iter_mut().enumerate() {
            *x = if width(i) == 0 { 1 << 60 } else { (1 << width(i)) / 2 - 1 };
        }
        big[N] = 1;
        let mut buf = vec![9];
        assert!(!enc.encode(&mut buf, 5, &big) && buf == [9] && enc.last.is_empty());
    }

    // the time and the sequence number wrap, a frame is late (no prediction past a second), the epoch and the clock
    // change, a far friend gets every fourth: each still comes back rounded
    #[test]
    fn the_edges_of_the_predictions() {
        let (mut enc, mut dec) = (Fpred::default(), Fpred::default());
        let mut seq = 65_530u16;
        let mut t = u32::MAX - 120_000;
        for k in 0..30i32 {
            seq = seq.wrapping_add(if k > 20 { 4 } else { 1 });
            t = t.wrapping_add(match k {
                10 => 3_000_000,
                11 => 0,
                _ => 50_000,
            });
            let mut b = body(9, seq, t, [-12000.5, 300.25, -2.0], [-60.0, 1.5, -0.25], k, if k % 2 == 0 { Some(16) } else { Some(28) });
            if k == 15 {
                b[9] = 8; // a teleport
                b[13] = 4; // a re-anchored clock
            }
            if k > 20 {
                b[6] |= 4; // every fourth
            }
            through(&mut enc, &mut dec, 0x0909, &b);
        }
    }

    // past REFS_MAX sources both ends forget them all at once, and still agree
    #[test]
    fn many_sources_both_ends_forget_together() {
        let (mut enc, mut dec) = (Fpred::default(), Fpred::default());
        for round in 0..3u32 {
            for s in 0..(REFS_MAX as u16 + 40) {
                let b = body(s as u32, round as u16, 1_000_000 * round + s as u32, [s as f32, 2.0, 3.0], [1.0, 0.0, 0.0], s as i32, Some(11));
                through(&mut enc, &mut dec, s, &b);
            }
            assert!(enc.last.len() <= REFS_MAX && enc.last.len() == dec.last.len());
        }
    }

    #[test]
    fn a_broken_entry_decodes_to_nothing() {
        let mut enc = Fpred::default();
        let b = body(1, 1, 1, [1.0; 3], [0.0; 3], 0, Some(16));
        let mut buf = Vec::new();
        assert!(enc.encode(&mut buf, 3, &fields(&b).unwrap()));
        let e = &buf[4..];
        assert!(Fpred::default().decode(3, &e[..e.len() - 1]).is_none(), "cut short");
        let mut more = e.to_vec();
        more.push(0);
        assert!(Fpred::default().decode(3, &more).is_none(), "one too many");
        assert!(Fpred::default().decode(3, &[0xFF; 9]).is_none(), "a bitmap past the fields");
    }
}
