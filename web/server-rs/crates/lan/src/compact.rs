//! Compact bundles (kind 33): the datagrams of a bundle to a page whose hello said "compact":1, without what the page
//! can put back exactly.  What reaches the game is the same, byte for byte; what goes over the network is about a
//! quarter less (a free-roam snapshot of 90 bytes cost 110 in a bundle, 84 here).  On nfsu2.xyz the port is the
//! limit (486 Mbit/s of 1000 at 2,960 players, 2026-10-22), and almost all of it is these snapshots.
//!
//! A bundle: [0] 33, [1..3] reserved, then entries of
//!   [0] flags: bits 0-2 the frame's kind (1-7); 0x08 to the broadcast address (else to the page's own); 0x10 the
//!       ports follow (else both are RC_DEFAULT_PORT, free roam's 47210); 0x20 a free-roam datagram without its
//!       "N2FR", version, length and CRC (the page writes them back, the CRC computed again: the switch checked it
//!       first); 0x40 the whole frame follows as it is (its 16-byte header included; the other bits 0)
//!   [1..2] u16 the source address's low 16 bits (10.77.x.y)
//!   [3..4] u16 the length of what follows: the ports (u16 source, u16 destination) when 0x10, then the data
//!
//! Left out per entry: the bundle entry's 4 bytes and the frame's 16-byte header for 5 (9 with ports); a free-roam
//! datagram's magic, version, length and CRC (11 bytes).  The page (web/src/net) puts the frame together again
//! before the game's mailbox sees it.
//!
//! To a page whose hello said "delta":1 too, a free-roam datagram to it on free roam's ports (0x20 without 0x08 and
//! 0x10) may go as what changed since the last one of its type from the same source (`Refs`): flags 0x80 with 0x20,
//! then its type, a bitmap of its body's bytes (bit i % 8 of byte i / 8: byte i of the body is new; the body as long
//! as the last one's), and the new bytes in order.  A friend's snapshots 20 times a second change in their position,
//! speed and turn, little else: a moving car's about 40 % fewer bytes, a standing one's about 75 % (an estimate from
//! the snapshot's fields, roam_core.c put_state).  Every such entry, in either form, is the last one of its source
//! and type for the next; the page keeps the same, entry for entry, and the stream (TCP) loses and reorders
//! nothing, so the game still gets each datagram byte for byte.
//!
//! To a page whose hello said "ldelta":1 besides, any other datagram to it (LAN play's UDP: the game's own races, Online's
//! and free roam's lobby races; UDP on other ports: the race horn) goes the same way, as what changed since the last one
//! of its kind from its source (`Refs::other`): flags 0x80 without 0x20, bits 0-2 its kind; 0x10 then says that its
//! length (u16) follows, for one not as long as the last one; then a bitmap of its bytes from its ports on (its ports,
//! LAN play's 8-byte header, its data: bit i % 8 of byte i / 8, a byte past the last one's end always new), and the new
//! bytes in order.  Every entry of such a datagram to the page's own address (not 0x20, 0x08 or 0x40) of at most
//! `OTHER_MAX` bytes from its ports on, plain or not, is the last one of its source and kind for the next, kept apart
//! from free roam's (REFS_MAX of each: a page that asks for them keeps free roam's just as a switch of before does).
//! Before, each went whole, every time, to each of a racer's rivals.

use std::collections::HashMap;
use std::hash::{BuildHasherDefault, Hasher};

use bytes::Bytes;

use crate::fpred::{self, Fpred};
use crate::{BASE, BROADCAST};

/// a compact bundle (page and switch only: the switch never routes one from a client)
pub(crate) const KIND_COMPACT: u8 = 33;
/// runtime/roam_core.h RC_DEFAULT_PORT
const ROAM_PORT: u16 = 47210;
const F_BCAST: u8 = 0x08;
const F_PORTS: u8 = 0x10;
const F_ROAM: u8 = 0x20;
const F_WHOLE: u8 = 0x40;
const F_DELTA: u8 = 0x80;
/// the longest other datagram (from its ports on) that goes as what changed and is kept as the last one (a longer one
/// goes as it is, kept by neither end): a race's are far shorter, and a page's last ones stay within 256 of these
pub(crate) const OTHER_MAX: usize = 1500;
/// the sources and types whose last bodies a connection keeps at most: a new one past that, and they all go first
/// (the page does the same, so both keep the same ones)
pub(crate) const REFS_MAX: usize = 256;
/// an entry's own bytes
const ENTRY: usize = 5;
/// "N2FR", version, length (left out at the front) and the CRC (at the end)
const ROAM_CUT: usize = 11;
/// a frame's byte 1 (reserved on the wire) in the switch's own copy of it: its datagram is a free-roam one whose
/// magic, version, length and CRC checked out (`mark`), so a compact bundle may leave them out
pub(crate) const ROAM_OK: u8 = 1;

/// runtime/roam_core.h: a free-roam datagram's type (its byte 5), a ping's and its answer's
const RC_SNAP: u8 = 3;
const RC_PING: u8 = 6;
const RC_PONG: u8 = 7;
/// the longest of them (a pong: its 16-byte frame header, the datagram's 16, three times and the CRC)
pub(crate) const TIMED_MAX: usize = 16 + 16 + 12 + 4;

/// a free-roam ping or pong to a page that takes compact bundles: queued with the time (conn.rs `Out::Dgram`), so that
/// its bundle can move its send time on by its wait (`waited`)
pub(crate) fn timed(data: &[u8]) -> bool {
    stamp_at(data).is_some()
}

/// where a ping's t0 or a pong's t2 is in its frame, when the frame holds it and the CRC after it: `mark` checks only
/// a datagram's length field and CRC, so a pong of 40 bytes is marked too, and its t2 would be past its end (the fourth
/// security audit's LAN-1: one such pong to the broadcast address panicked the writer of every compact page)
fn stamp_at(data: &[u8]) -> Option<usize> {
    if data.len() < 16 + 6 || data.len() > TIMED_MAX || data[1] != ROAM_OK {
        return None;
    }
    let at = 16 + match data[16 + 5] {
        RC_PING => 16,
        RC_PONG => 24,
        _ => return None,
    };
    (at + 4 + 4 <= data.len()).then_some(at)
}

/// A ping's send time (t0, which its answer brings back) or a pong's (t2) moved on by the time it waited for the bundle
/// clock, in a copy (`into`); anything else as it is.  The game draws a friend's solid car ahead by half the smallest
/// round trip of its last 8 pings (roam_game.c render_delay): a ping or pong that went with its player's car state
/// (the page holds them for it, web/src/net TX_HOLD_MS) waited for this clock about as long each time, and drew the
/// car up to 40 ms ahead (roam-pair, 2026-10-24: 0.16-0.31 m off on average against 0.06-0.09 m).  The CRC is left as
/// it was: an entry of free roam's leaves it out, and the page makes it again (`plan` gives every `timed` one F_ROAM).
pub(crate) fn waited<'a>(data: &'a [u8], us: u32, into: &'a mut [u8; TIMED_MAX]) -> &'a [u8] {
    let Some(at) = stamp_at(data) else { return data };
    if us == 0 || us >= 1_000_000 {
        return data;
    }
    let m = &mut into[..data.len()];
    m.copy_from_slice(data);
    let t =u32::from_le_bytes([m[at], m[at + 1], m[at + 2], m[at + 3]]).wrapping_add(us);
    m[at..at + 4].copy_from_slice(&t.to_le_bytes());
    m
}

/// a free-roam datagram (runtime/roam_core.c rc_encode) as the game's decoder takes it: "N2FR", version 1, its length
/// (u16 LE) the datagram's, the CRC-32 (zlib's, LE) of what comes before it
fn roam_ok(d: &[u8]) -> bool {
    let n = d.len();
    n >= 20
        && &d[..4] == b"N2FR"
        && d[4] == 1
        && u16::from_le_bytes([d[6], d[7]]) as usize == n
        && u32::from_le_bytes([d[n - 4], d[n - 3], d[n - 2], d[n - 1]]) == crc32fast::hash(&d[..n - 4])
}

/// the switch's copy of a browser's frame, with byte 1 saying whether its free-roam datagram may go compact (byte 1
/// is reserved on the wire: what a client put there is not kept; the writers send 0 there)
pub(crate) fn mark(data: &[u8]) -> Bytes {
    let mut v = data.to_vec();
    v[1] = if data[0] == crate::KIND_UDP && v.len() > 16 && roam_ok(&data[16..]) { ROAM_OK } else { 0 };
    Bytes::from(v)
}

/// an entry's flags and the bytes it takes, for a datagram of `data` from src to dst at a page holding `own`.  A frame
/// shorter than the 16 bytes of a header with ports goes whole: the reader takes any frame of 12 bytes or more, and
/// the compact form reads the ports and what follows them (the third audit: one 12-byte broadcast panicked the writer
/// of every page that takes compact bundles, which dropped them all)
fn plan(data: &[u8], src: u32, dst: u32, own: u32) -> (u8, usize) {
    let kind = data[0];
    if data.len() < 16 || kind == 0 || kind > 7 || src >> 16 != BASE >> 16 || (dst != own && dst != BROADCAST) {
        return (F_WHOLE, ENTRY + data.len());
    }
    let mut flags = kind;
    let mut n = ENTRY;
    if dst == BROADCAST {
        flags |= F_BCAST;
    }
    if u16::from_be_bytes([data[12], data[13]]) != ROAM_PORT || u16::from_be_bytes([data[14], data[15]]) != ROAM_PORT {
        flags |= F_PORTS;
        n += 4;
    }
    if data[1] == ROAM_OK {
        flags |= F_ROAM;
        n += data.len() - 16 - ROAM_CUT;
    } else {
        n += data.len() - 16;
    }
    (flags, n)
}

/// the bytes `entry` writes without deltas
#[cfg(test)]
pub(crate) fn entry_len(data: &[u8], src: u32, dst: u32, own: u32) -> usize {
    plan(data, src, dst, own).1
}

/// The last body (a free-roam datagram past its type, as an 0x20 entry carries it) of each source and type that went
/// to one page in a compact bundle: what its 0x80 entries are made against.  Its writer's own.
#[derive(Default)]
pub(crate) struct Refs {
    last: HashMap<u32, Vec<u8>, BuildHasherDefault<KeyHash>>,
    /// the same for the other datagrams (`other`), by source and kind: apart, so that a page that asks for them keeps
    /// free roam's as a switch of before does (REFS_MAX of each)
    others: HashMap<u32, Vec<u8>, BuildHasherDefault<KeyHash>>,
}

/// a key's hash: one multiply, folded (a map of at most `REFS_MAX` keys, made of a source and a type)
#[derive(Default)]
pub(crate) struct KeyHash(u64);

impl Hasher for KeyHash {
    fn finish(&self) -> u64 {
        self.0
    }
    fn write(&mut self, b: &[u8]) {
        for &x in b {
            self.write_u32(x as u32);
        }
    }
    fn write_u32(&mut self, k: u32) {
        let x = (self.0 ^ k as u64).wrapping_mul(0x9E37_79B9_7F4A_7C15);
        self.0 = x ^ x >> 32;
    }
}

impl Refs {
    /// the entry of a free-roam datagram's type and body from src: as what changed since that source's and type's
    /// last body when there is one as long and that is shorter, else as it is; body is their last now either way
    fn entry(&mut self, buf: &mut Vec<u8>, flags: u8, src: u32, kind: u8, body: &[u8], plain: impl FnOnce(&mut Vec<u8>)) {
        let key = (src & 0xFFFF) << 8 | kind as u32;
        match self.last.get_mut(&key) {
            Some(last) if last.len() == body.len() => {
                // in one pass: the bitmap, the new bytes after it, the last body brought up to date
                let start = buf.len();
                let map = body.len().div_ceil(8);
                buf.push(flags | F_DELTA);
                buf.extend_from_slice(&(src as u16).to_be_bytes());
                buf.extend_from_slice(&[0, 0, kind]);
                let at = buf.len();
                buf.resize(at + map, 0);
                for (i, (&b, l)) in body.iter().zip(last.iter_mut()).enumerate() {
                    if b != *l {
                        buf[at + i / 8] |= 1 << (i % 8);
                        buf.push(b);
                        *l = b;
                    }
                }
                let n = buf.len() - at + 1;
                if n < 1 + body.len() {
                    buf[start + 3..start + 5].copy_from_slice(&(n as u16).to_be_bytes());
                } else {
                    buf.truncate(start);
                    plain(buf);
                }
            }
            Some(last) => {
                last.clear();
                last.extend_from_slice(body);
                plain(buf);
            }
            None => {
                insert(&mut self.last, key, body);
                plain(buf);
            }
        }
    }

    /// The entry of another datagram from src (flags without F_ROAM, F_BCAST and F_WHOLE; `rest` its frame from its
    /// ports on): as what changed since that source's last one of its kind when there is one and that is shorter, else
    /// as it is; `rest` is their last now either way.
    fn other(&mut self, buf: &mut Vec<u8>, flags: u8, src: u32, rest: &[u8], plain: impl FnOnce(&mut Vec<u8>)) {
        let key = (src & 0xFFFF) << 8 | (flags & 7) as u32;
        let Some(last) = self.others.get_mut(&key) else {
            insert(&mut self.others, key, rest);
            return plain(buf);
        };
        let start = buf.len();
        let same = last.len() == rest.len();
        buf.push(flags & 7 | F_DELTA | if same { 0 } else { F_PORTS });
        buf.extend_from_slice(&(src as u16).to_be_bytes());
        buf.extend_from_slice(&[0, 0]);
        if !same {
            buf.extend_from_slice(&(rest.len() as u16).to_be_bytes());
        }
        let at = buf.len();
        buf.resize(at + rest.len().div_ceil(8), 0);
        for (i, &b) in rest.iter().enumerate() {
            if last.get(i) != Some(&b) {
                buf[at + i / 8] |= 1 << (i % 8);
                buf.push(b);
            }
        }
        // shorter than the plain entry (its 5 bytes, the ports when it carries them, the rest)
        let plain_len = ENTRY + rest.len() - if flags & F_PORTS != 0 { 0 } else { 4 };
        if buf.len() - start < plain_len {
            let n = (buf.len() - start - ENTRY) as u16;
            buf[start + 3..start + 5].copy_from_slice(&n.to_be_bytes());
        } else {
            buf.truncate(start);
            plain(buf);
        }
        last.clear();
        last.extend_from_slice(rest);
    }
}

/// a new source and type: past REFS_MAX of them, all go first (the page does the same)
fn insert(map: &mut HashMap<u32, Vec<u8>, BuildHasherDefault<KeyHash>>, key: u32, body: &[u8]) {
    if map.len() >= REFS_MAX {
        map.clear();
    }
    map.insert(key, body.to_vec());
}

/// one entry; with `refs` (a page that takes deltas) a free-roam datagram to it goes as what changed when that is
/// shorter, and is its source's last either way
#[cfg(test)]
pub(crate) fn entry(buf: &mut Vec<u8>, data: &[u8], src: u32, dst: u32, own: u32, refs: Option<&mut Refs>) {
    entry_fp(buf, data, src, dst, own, refs, None, false)
}

/// the same; with `fp` too (a page whose hello said "fpred":1) a friend's car state to it goes by its fields
/// (fpred.rs) when it can, and leaves `refs` alone then; with `others` (its hello said "ldelta":1) any other datagram to
/// it goes as what changed too (`Refs::other`)
#[allow(clippy::too_many_arguments)]
pub(crate) fn entry_fp(
    buf: &mut Vec<u8>,
    data: &[u8],
    src: u32,
    dst: u32,
    own: u32,
    refs: Option<&mut Refs>,
    fp: Option<&mut Fpred>,
    others: bool,
) {
    let (flags, n) = plan(data, src, dst, own);
    if let Some(fp) = fp
        && flags & (F_ROAM | F_PORTS | F_BCAST | F_WHOLE) == F_ROAM
        && data[16 + 5] == RC_SNAP
        && let Some(v) = fpred::fields(&data[16 + 8..data.len() - 4])
        && fp.encode(buf, src as u16, &v)
    {
        return;
    }
    match refs {
        Some(refs) if flags & (F_ROAM | F_PORTS | F_BCAST | F_WHOLE) == F_ROAM => {
            let d = &data[16..];
            refs.entry(buf, flags, src, d[5], &d[8..d.len() - 4], |buf| plain(buf, data, src, dst, flags, n));
        }
        Some(refs) if others && flags & (F_ROAM | F_BCAST | F_WHOLE) == 0 && data.len() - 12 <= OTHER_MAX => {
            refs.other(buf, flags, src, &data[12..], |buf| plain(buf, data, src, dst, flags, n));
        }
        _ => plain(buf, data, src, dst, flags, n),
    }
}

fn plain(buf: &mut Vec<u8>, data: &[u8], src: u32, dst: u32, flags: u8, n: usize) {
    buf.push(flags);
    if flags == F_WHOLE {
        buf.extend_from_slice(&[0, 0]);
        buf.extend_from_slice(&((n - ENTRY) as u16).to_be_bytes());
        crate::conn::with_addrs(buf, data, src, dst);
        return;
    }
    buf.extend_from_slice(&(src as u16).to_be_bytes());
    buf.extend_from_slice(&((n - ENTRY) as u16).to_be_bytes());
    if flags & F_PORTS != 0 {
        buf.extend_from_slice(&data[12..16]);
    }
    let d = &data[16..];
    if flags & F_ROAM != 0 {
        buf.push(d[5]);
        buf.extend_from_slice(&d[8..d.len() - 4]);
    } else {
        buf.extend_from_slice(d);
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// what the page does with a compact bundle (web/src/net/proto.ts unpackCompact): the frames as the game gets them
    pub(crate) fn unpack(b: &[u8], own: u32) -> Vec<Vec<u8>> {
        unpack_with(b, own, None)
    }

    /// the same for a page that takes deltas: refs its last bodies (by source and type), as `Refs` keeps them
    pub(crate) fn unpack_with(b: &[u8], own: u32, refs: Option<&mut HashMap<u32, Vec<u8>>>) -> Vec<Vec<u8>> {
        unpack_fp(b, own, refs, None)
    }

    /// a free-roam datagram ("N2FR", version 1, its type and length, the body (from its sender's id on), the CRC)
    fn dgram(kind: u8, body: &[u8]) -> Vec<u8> {
        let mut d = b"N2FR".to_vec();
        d.extend_from_slice(&[1, kind]);
        d.extend_from_slice(&((body.len() + 1 + ROAM_CUT) as u16).to_le_bytes());
        d.extend_from_slice(body);
        let crc = crc32fast::hash(&d);
        d.extend_from_slice(&crc.to_le_bytes());
        d
    }

    /// that in a frame from src to own on free roam's ports
    fn roam_frame(kind: u8, body: &[u8], src: u32, own: u32) -> Vec<u8> {
        frame(1, src, own, ROAM_PORT, ROAM_PORT, &dgram(kind, body))
    }

    /// the same for a page that takes car states by their fields too: `fp` the page's last ones (fpred.rs `decode`)
    pub(crate) fn unpack_fp(b: &[u8], own: u32, refs: Option<&mut HashMap<u32, Vec<u8>>>, fp: Option<&mut Fpred>) -> Vec<Vec<u8>> {
        unpack_all(b, own, refs, fp, None)
    }

    /// the same for a page that takes every other datagram as what changed too (its hello said "ldelta":1)
    pub(crate) fn unpack_all(
        b: &[u8],
        own: u32,
        mut refs: Option<&mut HashMap<u32, Vec<u8>>>,
        mut fp: Option<&mut Fpred>,
        mut others: Option<&mut HashMap<u32, Vec<u8>>>,
    ) -> Vec<Vec<u8>> {
        assert_eq!(b[0], KIND_COMPACT);
        let mut out = Vec::new();
        let mut at = 4;
        while at + ENTRY <= b.len() {
            let flags = b[at];
            if flags == fpred::F_FPRED {
                let src = u16::from_be_bytes([b[at + 1], b[at + 2]]);
                let end = at + 4 + b[at + 3] as usize;
                let body = fp.as_deref_mut().expect("a page that takes them").decode(src, &b[at + 4..end]).expect("it decodes");
                out.push(roam_frame(3, &body, BASE | src as u32, own));
                at = end;
                continue;
            }
            let src = BASE | u16::from_be_bytes([b[at + 1], b[at + 2]]) as u32;
            let n = u16::from_be_bytes([b[at + 3], b[at + 4]]) as usize;
            let mut p = at + ENTRY;
            let end = p + n;
            assert!(end <= b.len());
            if flags == F_WHOLE {
                out.push(b[p..end].to_vec());
                at = end;
                continue;
            }
            let other_key = (src & 0xFFFF) << 8 | (flags & 7) as u32;
            if flags & (F_DELTA | F_ROAM) == F_DELTA {
                let r = others.as_deref_mut().expect("only to a page that takes them");
                let last = r.get(&other_key).expect("its last one").clone();
                let len = if flags & F_PORTS != 0 {
                    p += 2;
                    u16::from_be_bytes([b[p - 2], b[p - 1]]) as usize
                } else {
                    last.len()
                };
                let mut next = p + len.div_ceil(8);
                let rest: Vec<u8> = (0..len)
                    .map(|i| {
                        if b[p + i / 8] >> (i % 8) & 1 == 1 {
                            next += 1;
                            b[next - 1]
                        } else {
                            last[i]
                        }
                    })
                    .collect();
                assert_eq!(next, end, "the new bytes fill the entry");
                let mut f = vec![flags & 7, 0, 0, 0];
                f.extend_from_slice(&src.to_be_bytes());
                f.extend_from_slice(&own.to_be_bytes());
                f.extend_from_slice(&rest);
                r.insert(other_key, rest);
                out.push(f);
                at = end;
                continue;
            }
            let mut f = vec![flags & 7, 0, 0, 0];
            f.extend_from_slice(&src.to_be_bytes());
            f.extend_from_slice(&(if flags & F_BCAST != 0 { BROADCAST } else { own }).to_be_bytes());
            if flags & F_PORTS != 0 {
                f.extend_from_slice(&b[p..p + 4]);
                p += 4;
            } else {
                f.extend_from_slice(&ROAM_PORT.to_be_bytes());
                f.extend_from_slice(&ROAM_PORT.to_be_bytes());
            }
            if flags & F_ROAM != 0 {
                let kind = b[p];
                let key = (src & 0xFFFF) << 8 | kind as u32;
                let body = if flags & F_DELTA != 0 {
                    let last = refs.as_ref().unwrap().get(&key).expect("a delta's last body").clone();
                    let map = last.len().div_ceil(8);
                    let mut next = p + 1 + map;
                    let body: Vec<u8> = (0..last.len())
                        .map(|i| {
                            if b[p + 1 + i / 8] >> (i % 8) & 1 == 1 {
                                next += 1;
                                b[next - 1]
                            } else {
                                last[i]
                            }
                        })
                        .collect();
                    assert_eq!(next, end, "the new bytes fill the entry");
                    body
                } else {
                    b[p + 1..end].to_vec()
                };
                if let Some(r) = refs.as_deref_mut().filter(|_| flags & (F_PORTS | F_BCAST) == 0) {
                    if r.len() >= REFS_MAX && !r.contains_key(&key) {
                        r.clear();
                    }
                    r.insert(key, body.clone());
                }
                let len = body.len() + 1 + ROAM_CUT;
                let mut d = b"N2FR".to_vec();
                d.push(1);
                d.push(kind);
                d.extend_from_slice(&(len as u16).to_le_bytes());
                d.extend_from_slice(&body);
                let crc = crc32fast::hash(&d);
                d.extend_from_slice(&crc.to_le_bytes());
                f.extend_from_slice(&d);
            } else {
                f.extend_from_slice(&b[p..end]);
                if let Some(r) = others.as_deref_mut().filter(|_| flags & F_BCAST == 0 && f.len() - 12 <= OTHER_MAX) {
                    if r.len() >= REFS_MAX && !r.contains_key(&other_key) {
                        r.clear();
                    }
                    r.insert(other_key, f[12..].to_vec());
                }
            }
            out.push(f);
            at = end;
        }
        out
    }

    /// a free-roam datagram as rc_encode makes it: a type, a body, the length and CRC filled in
    pub(crate) fn roam(kind: u8, body: &[u8]) -> Vec<u8> {
        let mut d = b"N2FR".to_vec();
        d.extend_from_slice(&[1, kind, 0, 0]);
        d.extend_from_slice(&0x1234_5678u32.to_le_bytes());
        d.extend_from_slice(&[7, 0, 0, 0]);
        d.extend_from_slice(body);
        let n = d.len() + 4;
        d[6..8].copy_from_slice(&(n as u16).to_le_bytes());
        let crc = crc32fast::hash(&d);
        d.extend_from_slice(&crc.to_le_bytes());
        d
    }

    /// a frame of `kind` as a browser sends it (its source and destination as the switch writes them going out)
    fn frame(kind: u8, src: u32, dst: u32, sport: u16, dport: u16, data: &[u8]) -> Vec<u8> {
        let mut f = vec![kind, 0, 0, 0];
        f.extend_from_slice(&src.to_be_bytes());
        f.extend_from_slice(&dst.to_be_bytes());
        f.extend_from_slice(&sport.to_be_bytes());
        f.extend_from_slice(&dport.to_be_bytes());
        f.extend_from_slice(data);
        f
    }

    // every kind of entry comes back as the frame it was, byte for byte, and the usual one is a quarter smaller
    #[test]
    fn entries_come_back_byte_for_byte() {
        let own = BASE | 0x0305;
        let (a, b) = (BASE | 0x0102, BASE | 0x0F07);
        let snap = roam(3, &[9u8; 70]);
        let mut broken = roam(3, &[9u8; 70]);
        *broken.last_mut().unwrap() ^= 1;
        let cases: Vec<(Vec<u8>, u32, u32)> = vec![
            // a snapshot to us on free roam's ports: the compact form
            (frame(1, a, own, ROAM_PORT, ROAM_PORT, &snap), a, own),
            // a broadcast (discovery without a hub)
            (frame(1, b, BROADCAST, ROAM_PORT, ROAM_PORT, &roam(1, b"hello")), b, BROADCAST),
            // other ports (a second game on one browser, the Roam port setting)
            (frame(1, a, own, 47211, 47300, &snap), a, own),
            // a CRC that does not check out, not a free-roam datagram, a short one: their bytes as they are
            (frame(1, a, own, ROAM_PORT, ROAM_PORT, &broken), a, own),
            (frame(1, a, own, ROAM_PORT, ROAM_PORT, b"just some bytes of a datagram"), a, own),
            (frame(1, a, own, ROAM_PORT, ROAM_PORT, b""), a, own),
            // LAN play's UDP and ICMP, and the test loopback
            (frame(3, b, own, 6112, 6113, &[1, 64, 0, 0, 0, 0, 0, 7, 42, 42]), b, own),
            (frame(5, b, BROADCAST, 0, 0, &[8, 64, 0, 0, 0, 0, 0, 1]), b, BROADCAST),
            (frame(2, a, BROADCAST, 47300, 47300, &snap), a, BROADCAST),
            // a source outside 10.77/16 (none on the switch today) and a destination that is neither: whole
            (frame(1, 0x0B00_0001, own, ROAM_PORT, ROAM_PORT, &snap), 0x0B00_0001, own),
            (frame(1, a, BASE | 9, ROAM_PORT, ROAM_PORT, &snap), a, BASE | 9),
        ];
        let mut b = vec![KIND_COMPACT, 0, 0, 0];
        let mut want = Vec::new();
        for (f, src, dst) in &cases {
            let m = mark(f);
            let before = b.len();
            entry(&mut b, &m, *src, *dst, own, None);
            assert_eq!(b.len() - before, entry_len(&m, *src, *dst, own));
            want.push(f.clone());
        }
        assert_eq!(unpack(&b, own), want);
        // the snapshot: 90 bytes of datagram, 110 in a bundle of before, 84 here
        let m = mark(&cases[0].0);
        assert_eq!(snap.len(), 90);
        assert_eq!(entry_len(&m, a, own, own), 84);
        assert_eq!(mark(&cases[3].0)[1], 0, "a broken CRC is not left out");
    }

    // a frame too short for its ports (12 to 15 bytes: the reader takes them) goes whole, broadcast or not
    #[test]
    fn a_frame_without_ports_goes_whole() {
        let (own, a) = (BASE | 0x0305, BASE | 0x0102);
        let mut b = vec![KIND_COMPACT, 0, 0, 0];
        let mut want = Vec::new();
        for (kind, dst, len) in [(1u8, BROADCAST, 12usize), (1, own, 15), (3, BROADCAST, 13), (5, own, 14)] {
            let mut f = vec![kind, 0, 0, 0];
            f.extend_from_slice(&a.to_be_bytes());
            f.extend_from_slice(&dst.to_be_bytes());
            f.resize(len, 0xAB);
            let m = mark(&f);
            let before = b.len();
            entry(&mut b, &m, a, dst, own, None);
            assert_eq!(b.len() - before, entry_len(&m, a, dst, own));
            assert_eq!(b[before], F_WHOLE);
            want.push(f);
        }
        assert_eq!(unpack(&b, own), want);
    }

    // to a page that takes deltas: every datagram comes back byte for byte (snapshots changing a few bytes at a time,
    // other types between them, a longer body, ports, broadcasts, LAN play's, bundles cut where they fill), and a
    // snapshot that changed a little takes far fewer bytes
    #[test]
    fn deltas_come_back_byte_for_byte() {
        let own = BASE | 0x0305;
        let srcs = [BASE | 0x0102, BASE | 0x0F07, BASE | 0x1301];
        let mut seed = 0x9E37_79B9u32;
        let mut rnd = move || {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            seed
        };
        let mut bodies: Vec<Vec<u8>> = srcs.iter().map(|_| (0..75).map(|_| rnd() as u8).collect()).collect();
        let mut frames = Vec::new();
        for step in 0..400 {
            let i = rnd() as usize % srcs.len();
            let body = &mut bodies[i];
            // a snapshot: a few of its bytes new (a moving car's position and speed), now and then a longer one
            for _ in 0..rnd() % 30 {
                let k = rnd() as usize % body.len();
                body[k] = rnd() as u8;
            }
            if step % 97 == 0 {
                body.push(rnd() as u8);
            }
            frames.push((frame(1, srcs[i], own, ROAM_PORT, ROAM_PORT, &roam(3, body)), srcs[i], own));
            match step % 13 {
                0 => frames.push((frame(1, srcs[i], own, ROAM_PORT, ROAM_PORT, &roam(1, b"a hello")), srcs[i], own)),
                5 => frames.push((frame(1, srcs[i], BROADCAST, ROAM_PORT, ROAM_PORT, &roam(3, body)), srcs[i], BROADCAST)),
                7 => frames.push((frame(1, srcs[i], own, 47211, 47211, &roam(3, body)), srcs[i], own)),
                9 => frames.push((frame(3, srcs[i], own, 6112, 6113, &[1, 64, 0, 0, 0, 0, 0, 7, 42]), srcs[i], own)),
                _ => {}
            }
        }
        // in bundles of about 2 KB, each made after the last as the writer makes them
        let mut refs = Refs::default();
        let (mut bundles, mut b) = (Vec::new(), vec![KIND_COMPACT, 0, 0, 0]);
        let (mut plain_bytes, mut delta_bytes) = (0, 0);
        for (f, src, dst) in &frames {
            let m = mark(f);
            let at = b.len();
            entry(&mut b, &m, *src, *dst, own, Some(&mut refs));
            plain_bytes += entry_len(&m, *src, *dst, own);
            delta_bytes += b.len() - at;
            if b.len() > 2000 {
                bundles.push(b);
                b = vec![KIND_COMPACT, 0, 0, 0];
            }
        }
        bundles.push(b);
        let mut page = HashMap::new();
        let got: Vec<Vec<u8>> = bundles.iter().flat_map(|b| unpack_with(b, own, Some(&mut page))).collect();
        assert_eq!(got.len(), frames.len());
        for (g, (f, ..)) in got.iter().zip(&frames) {
            assert_eq!(g, f);
        }
        assert!(delta_bytes * 10 < plain_bytes * 7, "{delta_bytes} bytes with deltas, {plain_bytes} without");
    }

    // to a page that takes them, races' datagrams (LAN play's UDP from 5 rivals, a few bytes new each time, now and then
    // of another length or on other ports), the race horn's UDP, LAN broadcasts, free roam's among them and sources
    // enough to clear the last ones now and then: what changed goes, and each comes back byte for byte
    #[test]
    fn other_datagrams_as_what_changed_come_back_byte_for_byte() {
        let own = BASE | 0x0305;
        let mut seed = 0x2545_F491u32;
        let mut rnd = move || {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            seed
        };
        let rivals: Vec<u32> = (0..5u32).map(|i| BASE | (0x0400 + i)).collect();
        let mut packets: Vec<Vec<u8>> = rivals.iter().map(|_| (0..120).map(|_| rnd() as u8).collect()).collect();
        let mut frames: Vec<(Vec<u8>, u32, u32)> = Vec::new();
        for step in 0..4000u32 {
            let i = rnd() as usize % rivals.len();
            let (src, p) = (rivals[i], &mut packets[i]);
            for _ in 0..rnd() % 12 {
                let k = rnd() as usize % p.len();
                p[k] = rnd() as u8;
            }
            match step % 61 {
                0 => p.push(rnd() as u8),
                30 => {
                    p.pop();
                }
                _ => {}
            }
            let mut d = vec![0, 64, 0, 0, 0, 0, 0, 0];
            d.extend_from_slice(p);
            let ports = if step % 89 == 0 { (6112, 6114) } else { (6112, 6113) };
            frames.push((frame(3, src, own, ports.0, ports.1, &d), src, own));
            match step % 17 {
                0 => frames.push((frame(1, src, own, 3659, 3659, &[1, 2, 3, (step % 3) as u8]), src, own)),
                4 => frames.push((frame(3, src, BROADCAST, 6112, 6112, &[0, 1, 0, 0, 0, 0, 0, 0, 9, 9]), src, BROADCAST)),
                8 => frames.push((frame(1, src, own, ROAM_PORT, ROAM_PORT, &roam(3, &p[..60])), src, own)),
                3 => frames.push((frame(3, src, own, 6112, 6113, &vec![(step % 7) as u8; 1600]), src, own)),
                12 | 14 => {
                    // a stranger: past REFS_MAX of them both ends clear their last ones
                    let s = BASE | (0x1000 + step);
                    frames.push((frame(3, s, own, 6112, 6113, &d), s, own));
                }
                _ => {}
            }
        }
        let mut refs = Refs::default();
        let (mut bundles, mut b) = (Vec::new(), vec![KIND_COMPACT, 0, 0, 0]);
        let (mut plain_bytes, mut delta_bytes, mut as_deltas) = (0, 0, 0);
        for (f, src, dst) in &frames {
            let m = mark(f);
            let at = b.len();
            entry_fp(&mut b, &m, *src, *dst, own, Some(&mut refs), None, true);
            if m[0] == 3 && *dst == own && rivals.contains(src) && m.len() - 12 <= OTHER_MAX {
                plain_bytes += entry_len(&m, *src, *dst, own);
                delta_bytes += b.len() - at;
                as_deltas += (b[at] & (F_DELTA | F_ROAM) == F_DELTA) as usize;
            }
            if b.len() > 2000 {
                bundles.push(b);
                b = vec![KIND_COMPACT, 0, 0, 0];
            }
        }
        bundles.push(b);
        let (mut page, mut page_others) = (HashMap::new(), HashMap::new());
        let got: Vec<Vec<u8>> = bundles.iter().flat_map(|b| unpack_all(b, own, Some(&mut page), None, Some(&mut page_others))).collect();
        assert_eq!(got.len(), frames.len());
        for (k, (g, (f, ..))) in got.iter().zip(&frames).enumerate() {
            assert_eq!(g, f, "frame {k}");
        }
        assert!(as_deltas > 3500, "{as_deltas} as deltas");
        assert!(delta_bytes * 10 < plain_bytes * 4, "{delta_bytes} bytes with deltas, {plain_bytes} without");
        // without "ldelta" the same go as before
        let mut b = vec![KIND_COMPACT, 0, 0, 0];
        let mut refs = Refs::default();
        for (f, src, dst) in frames.iter().take(200) {
            let m = mark(f);
            entry(&mut b, &m, *src, *dst, own, Some(&mut refs));
        }
        let got = unpack_with(&b, own, Some(&mut HashMap::new()));
        assert!(got.iter().zip(&frames).all(|(g, (f, ..))| g == f));
    }

    // a ping's t0 and a pong's t2 come to the page moved on by their wait, with a CRC that checks out; the rest of them,
    // a snapshot, and one that did not wait come as they were
    #[test]
    fn a_ping_and_a_pong_come_with_their_wait_counted_out() {
        let (own, src) = (BASE | 0x0305, BASE | 0x0102);
        let ping = mark(&frame(1, src, own, ROAM_PORT, ROAM_PORT, &roam(RC_PING, &1_000_000u32.to_le_bytes())));
        let pong_body: Vec<u8> = [500u32, 7000, 7000].iter().flat_map(|t| t.to_le_bytes()).collect();
        let pong = mark(&frame(1, src, own, ROAM_PORT, ROAM_PORT, &roam(RC_PONG, &pong_body)));
        let snap = mark(&frame(1, src, own, ROAM_PORT, ROAM_PORT, &roam(3, &[9; 52])));
        assert!(timed(&ping) && timed(&pong) && !timed(&snap));
        let mut into = [0u8; TIMED_MAX];
        let mut b = vec![KIND_COMPACT, 0, 0, 0];
        let mut refs = Refs::default();
        for (f, us) in [(&ping, 2500), (&pong, 31_000), (&snap, 40_000), (&ping, 0)] {
            let d = waited(f, us, &mut into).to_vec();
            entry(&mut b, &d, src, own, own, Some(&mut refs));
        }
        let got = unpack_with(&b, own, Some(&mut HashMap::new()));
        let t = |g: &Vec<u8>, at: usize| u32::from_le_bytes(g[16 + at..16 + at + 4].try_into().unwrap());
        assert_eq!(t(&got[0], 16), 1_002_500, "the ping's t0");
        assert_eq!((t(&got[1], 16), t(&got[1], 20), t(&got[1], 24)), (500, 7000, 38_000), "the pong's t0, t1, t2");
        for g in &got {
            assert!(roam_ok(&g[16..]), "a CRC that checks out");
        }
        assert_eq!(got[2], snap.to_vec().iter().enumerate().map(|(i, &x)| if i == 1 { 0 } else { x }).collect::<Vec<u8>>());
        assert_eq!(t(&got[3], 16), 1_000_000, "no wait, no change");
    }

    // a ping or pong of every length the switch marks: one too short for its time comes as it was, never read or
    // written past its end (the fourth security audit's LAN-1: a pong of 40 bytes panicked the writer)
    #[test]
    fn a_ping_or_pong_too_short_for_its_time_comes_as_it_was() {
        let (own, src) = (BASE | 0x0305, BASE | 0x0102);
        let mut into = [0u8; TIMED_MAX];
        for (kind, times) in [(RC_PING, 4), (RC_PONG, 12)] {
            for n in 0..=12 {
                let f = mark(&frame(1, src, BROADCAST, ROAM_PORT, ROAM_PORT, &roam(kind, &vec![7; n])));
                assert_eq!(f[1], ROAM_OK, "marked: {n} bytes");
                assert_eq!(timed(&f), n >= times, "type {kind}, {n} bytes");
                let d = waited(&f, 2500, &mut into).to_vec();
                if n < times {
                    assert_eq!(d, f.to_vec(), "type {kind}, {n} bytes: as it was");
                }
                let mut b = vec![KIND_COMPACT, 0, 0, 0];
                entry(&mut b, &d, src, BROADCAST, own, Some(&mut Refs::default()));
                let got = unpack_with(&b, own, Some(&mut HashMap::new()));
                assert_eq!(got.len(), 1);
                assert!(roam_ok(&got[0][16..]), "type {kind}, {n} bytes: a CRC that checks out");
            }
        }
    }

    // to a page that takes car states by their fields: a friend's SNAP goes so and reaches the game rounded, everything
    // else (a ping with its wait, a HELLO, a SNAP of three states, one to the broadcast address) as before, byte for byte
    #[test]
    fn car_states_by_their_fields_among_the_rest() {
        use crate::fpred::tests::body;
        let (own, a, b) = (BASE | 0x0305, BASE | 0x0102, BASE | 0x0207);
        let (mut refs, mut fp) = (Refs::default(), Fpred::default());
        let (mut page_refs, mut page_fp) = (HashMap::new(), Fpred::default());
        let mut want: Vec<Vec<u8>> = Vec::new();
        let mut bundle = vec![KIND_COMPACT, 0, 0, 0];
        let mut into = [0u8; TIMED_MAX];
        let mut put = |bundle: &mut Vec<u8>, f: Vec<u8>, src: u32, dst: u32, us: u32, want: &mut Vec<Vec<u8>>, rounded: bool| {
            let m = mark(&f);
            let d = waited(&m, us, &mut into).to_vec();
            entry_fp(bundle, &d, src, dst, own, Some(&mut refs), Some(&mut fp), false);
            let mut w = d.clone();
            w[1] = 0;
            if rounded {
                let body = fpred::rounded(&w[16 + 8..w.len() - 4]).unwrap();
                w = roam_frame(3, &body, src, own);
            } else if us > 0 {
                let n = w.len();
                let crc = crc32fast::hash(&w[16..n - 4]);
                w[n - 4..].copy_from_slice(&crc.to_le_bytes());
            }
            want.push(w);
        };
        for k in 0..40i32 {
            let t = 1_000_000 + 50_000 * k as u32;
            for (src, x) in [(a, Some(16)), (b, Some(28))] {
                let s = body(src, k as u16, t, [100.0 + k as f32 * 1.5, 2.0, 3.0], [30.0, 0.5, 0.0], k, x);
                put(&mut bundle, roam_frame(3, &s, src, own), src, own, 0, &mut want, true);
            }
            if k % 5 == 0 {
                put(&mut bundle, frame(1, a, own, ROAM_PORT, ROAM_PORT, &roam(RC_PING, &(k as u32).to_le_bytes())), a, own, 3000, &mut want, false);
                put(&mut bundle, frame(1, b, own, ROAM_PORT, ROAM_PORT, &roam(1, b"a hello")), b, own, 0, &mut want, false);
                let mut three = body(a, k as u16, t, [1.0; 3], [0.0; 3], k, Some(16));
                three[12] = 3;
                put(&mut bundle, roam_frame(3, &three, a, own), a, own, 0, &mut want, false);
                let s = body(b, k as u16, t, [5.0; 3], [0.0; 3], k, Some(16));
                put(&mut bundle, frame(1, b, BROADCAST, ROAM_PORT, ROAM_PORT, &dgram(3, &s)), b, BROADCAST, 0, &mut want, false);
            }
        }
        let got = unpack_fp(&bundle, own, Some(&mut page_refs), Some(&mut page_fp));
        assert_eq!(got.len(), want.len());
        for (i, (g, w)) in got.iter().zip(&want).enumerate() {
            assert_eq!(g, w, "frame {i}");
        }
    }

    // past REFS_MAX sources the writer's last bodies all go, and the page's with them: still byte for byte
    #[test]
    fn many_sources_the_last_bodies_go_together() {
        let own = BASE | 0x0305;
        let mut refs = Refs::default();
        let mut page = HashMap::new();
        for round in 0..3u8 {
            let mut b = vec![KIND_COMPACT, 0, 0, 0];
            let mut want = Vec::new();
            for s in 0..(REFS_MAX as u32 + 40) {
                let src = BASE | (s / 250) << 8 | (s % 250 + 2);
                let f = frame(1, src, own, ROAM_PORT, ROAM_PORT, &roam(3, &[round, s as u8, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7]));
                entry(&mut b, &mark(&f), src, own, own, Some(&mut refs));
                want.push(f);
            }
            assert_eq!(unpack_with(&b, own, Some(&mut page)), want, "round {round}");
            assert!(refs.last.len() <= REFS_MAX && page.len() == refs.last.len());
        }
    }

    /// what a friend's moving car state costs the writer as a byte delta and by its fields (fpred.rs): 32 friends driving
    /// circles, 20 states a second each, in turn
    #[test]
    #[ignore]
    fn what_a_car_state_costs() {
        use crate::fpred::tests::body;
        let own = BASE | 0x0305;
        let frames: Vec<(Vec<u8>, u32)> = (0..200i32)
            .flat_map(|k| {
                (0..32u32).map(move |s| {
                    let src = BASE | (s + 2);
                    let a = k as f64 * 0.05 + s as f64;
                    let p = [(1000.0 + 40.0 * a.cos()) as f32, (-500.0 + 40.0 * a.sin()) as f32, 24.0 + 0.01 * k as f32];
                    let v = [(-40.0 * a.sin()) as f32, (40.0 * a.cos()) as f32, 0.1];
                    let b = body(src, k as u16, 1_000_000 + 50_000 * k as u32 + s, p, v, k, Some(16));
                    (mark(&roam_frame(3, &b, src, own)).to_vec(), src)
                })
            })
            .collect();
        for fp_on in [false, true] {
            let (mut r, mut fp) = (Refs::default(), Fpred::default());
            let mut b = Vec::with_capacity(64 << 10);
            let (t, mut bytes) = (std::time::Instant::now(), 0);
            let n = frames.len() * 50;
            for i in 0..n {
                let (f, src) = &frames[i % frames.len()];
                if i % frames.len() == 0 {
                    (r, fp) = (Refs::default(), Fpred::default());
                }
                if b.len() > 30_000 {
                    bytes += b.len();
                    b.clear();
                }
                entry_fp(&mut b, f, *src, own, own, Some(&mut r), fp_on.then_some(&mut fp), false);
            }
            bytes += b.len();
            println!("by fields {fp_on}: {:.0} ns an entry, {:.1} bytes", t.elapsed().as_secs_f64() * 1e9 / n as f64, bytes as f64 / n as f64);
        }
    }

    /// what friends' car states cost on real drives: a capture of the frames the switch sent (NFSU2_CAPTURE=<file>:
    /// [t_us u64 LE][len u16 LE][frame], each as its receiver got it), through the writer as a byte delta and by their
    /// fields, each receiver with its own last bodies and states
    #[test]
    #[ignore]
    fn what_car_states_cost_on_a_capture() {
        let Ok(path) = std::env::var("NFSU2_CAPTURE") else { return };
        let cap = std::fs::read(path).unwrap();
        let mut frames = Vec::new();
        let mut o = 0;
        while o + 10 <= cap.len() {
            let len = u16::from_le_bytes([cap[o + 8], cap[o + 9]]) as usize;
            frames.push(&cap[o + 10..o + 10 + len]);
            o += 10 + len;
        }
        for fp_on in [false, true] {
            let mut ends: HashMap<u32, (Refs, Fpred)> = HashMap::new();
            let (mut snap, mut snaps, mut rest, mut rests, mut by_fields) = (0, 0, 0, 0, 0);
            for f in &frames {
                let (src, dst) = (u32::from_be_bytes([f[4], f[5], f[6], f[7]]), u32::from_be_bytes([f[8], f[9], f[10], f[11]]));
                if dst & 0xFFFF_0000 != BASE || dst == BASE | 0xFD {
                    continue;
                }
                let m = mark(f);
                let (r, fp) = ends.entry(dst).or_default();
                let mut b = Vec::new();
                entry_fp(&mut b, &m, src, dst, dst, Some(r), fp_on.then_some(fp), true);
                if m.len() > 21 && m[1] == ROAM_OK && m[16 + 5] == 3 {
                    (snap, snaps) = (snap + b.len(), snaps + 1);
                    by_fields += (b[0] == fpred::F_FPRED) as usize;
                } else {
                    (rest, rests) = (rest + b.len(), rests + 1);
                }
            }
            println!(
                "by fields {fp_on}: {snaps} car states {:.1} bytes each ({by_fields} by their fields), {rests} others {:.1} bytes each",
                snap as f64 / snaps.max(1) as f64,
                rest as f64 / rests.max(1) as f64
            );
        }
    }

    /// what an entry costs the writer, with deltas and without (a snapshot of 75 body bytes, 20 of them new each time,
    /// from 32 friends)
    #[test]
    #[ignore]
    fn what_an_entry_costs() {
        let own = BASE | 0x0305;
        let frames: Vec<(Vec<u8>, u32)> = (0..32u32)
            .flat_map(|s| {
                let src = BASE | (s + 2);
                (0..8u8).map(move |k| {
                    let body: Vec<u8> = (0..75u8).map(|i| if i % 4 == k % 4 { i ^ k } else { i }).collect();
                    (mark(&frame(1, src, own, ROAM_PORT, ROAM_PORT, &roam(3, &body))).to_vec(), src)
                })
            })
            .collect();
        for refs in [false, true] {
            let mut r = Refs::default();
            let mut b = Vec::with_capacity(64 << 10);
            let (t, mut bytes) = (std::time::Instant::now(), 0);
            let n = 1_000_000;
            for i in 0..n {
                let (f, src) = &frames[i % frames.len()];
                if b.len() > 30_000 {
                    bytes += b.len();
                    b.clear();
                }
                entry(&mut b, f, *src, own, own, refs.then_some(&mut r));
            }
            bytes += b.len();
            println!("deltas {refs}: {:.0} ns an entry, {:.1} bytes", t.elapsed().as_secs_f64() * 1e9 / n as f64, bytes as f64 / n as f64);
        }
    }

    // byte 1 of the switch's copy is its own: what a client wrote there never says "checked"
    #[test]
    fn a_clients_byte_1_is_not_kept() {
        let mut f = frame(1, BASE | 2, BASE | 3, ROAM_PORT, ROAM_PORT, b"not free roam");
        f[1] = ROAM_OK;
        assert_eq!(mark(&f)[1], 0);
        let mut g = frame(3, BASE | 2, BASE | 3, 1, 2, &roam(3, &[0; 8]));
        g[1] = ROAM_OK;
        assert_eq!(mark(&g)[1], 0, "only kind 1 is free roam's");
    }
}
