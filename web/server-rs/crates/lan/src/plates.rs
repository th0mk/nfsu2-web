//! Players' license plates in free roam (runtime/plates.c): a HELLO to a peer carries the player's plate after its
//! flag, with the header flag RC_HF_PLATE (runtime/roam_core.h): up to 8 capitals and digits that every other player
//! near it sees on its car.  The switch is where all of them pass, so here (route.rs, every frame a page sends,
//! bundled or not) a plate HELLO goes on, but
//!  - one whose plate is not capitals and digits, or is cut short, goes on without it (as a game's own decoder would
//!    show none);
//!  - one whose plate reads as a slur (nfsu2_names, as a player's name) goes on without it, and a `[plate]` line says
//!    so (once for each plate of the player): the operator's tools take the player by the name the line gives (`ctl
//!    who`, `kick`, `ban`, as for a name).  The player is heard as before: a game whose word list is older than the
//!    server's (a page or module of before a new round of the name filter) passes its own plate, and only the plate
//!    goes;
//!  - one whose plate is another than the player's last one within `EVERY_MS` of it goes on with the last one until
//!    then, so a player who changes plates again and again costs the others a texture's pixels every 10 s at most;
//!    each plate that goes is a `[plate]` line.
//!
//! The HELLO is rewritten for that (`rewrite`: the plate's bytes, the header's flag, the length, the CRC), and only
//! one whose length and CRC are right: one that is not goes as it came, and every game drops it.  A HELLO without the
//! flag (an older game's, a player without a plate) is not looked at.

use parking_lot::Mutex;

/// a player's plate changes for the others at most this often (runtime/roam_game.c PLATE_EVERY_US)
pub(crate) const EVERY_MS: i64 = 10_000;
/// runtime/roam_core.h RC_PLATE_MAX
pub(crate) const MAX: usize = 8;

const HF_CREW: u16 = 0x0200;
const HF_PLATE: u16 = 0x8000;
/// the switch frame's header before the datagram: kind, addresses, ports
const FRAME_HDR: usize = 16;

/// A plate HELLO's plate, as `hello_plate` found it
#[derive(Debug, PartialEq)]
pub(crate) enum Found<'a> {
    /// its plate: at `at` in the datagram (its length byte), with the name over the player's car
    Plate { at: usize, plate: &'a [u8], name: &'a [u8] },
    /// one whose plate is cut short or not capitals and digits (or not where the flag says)
    Broken,
}

/// A free-roam frame's HELLO plate: None for any other frame (`frame`: a switch frame, the datagram after its header)
pub(crate) fn hello_plate(frame: &[u8]) -> Option<Found<'_>> {
    let d = frame.get(FRAME_HDR..)?;
    if frame[0] != crate::KIND_UDP || d.len() < 29 + 4 || &d[..4] != b"N2FR" || d[4] != 1 || d[5] != 1 {
        return None;
    }
    let flags = u16::from_le_bytes([d[14], d[15]]);
    if flags & HF_PLATE == 0 {
        return None;
    }
    Some(parse(&d[..d.len() - 4], flags).unwrap_or(Found::Broken))
}

/// After the 16-byte header: core 2, platform, status, port 2, cfg 2, room 4, the name (its length, then it), the ids
/// it hears (their count, then 4 each), then with RC_HF_CREW the crews (their count, 4 each), the links (1), the outrun's
/// player (4), the flag (its length, then it) and the plate (its length, then it): roam_core.c rc_encode
fn parse(d: &[u8], flags: u16) -> Option<Found<'_>> {
    if flags & HF_CREW == 0 {
        return None;
    }
    let nl = *d.get(28)? as usize;
    let name = d.get(29..29 + nl)?;
    let mut at = 29 + nl;
    let seen = *d.get(at)? as usize;
    at += 1 + 4 * seen;
    let crews = *d.get(at)? as usize;
    at += 1 + 4 * crews + 1 + 4;
    let flag = *d.get(at)? as usize;
    at += 1 + flag;
    let n = *d.get(at)? as usize;
    let plate = d.get(at + 1..at + 1 + n)?;
    (seen <= 64 && crews <= 4 && flag <= 6 && (1..=MAX).contains(&n) && ok(plate)).then_some(Found::Plate { at, plate, name })
}

/// capitals and digits only (runtime/roam_core.c rc_plate_ok)
pub(crate) fn ok(plate: &[u8]) -> bool {
    plate.len() <= MAX && plate.iter().all(|b| b.is_ascii_uppercase() || b.is_ascii_digit())
}

/// A plate HELLO's frame with `plate` in place of its own (the plate's bytes at `at`), or without one (`plate` None:
/// the header's flag off, the plate's bytes gone; `at` None: a plate not found, only the flag off); the datagram's
/// length and CRC made right.  None: the frame's length or CRC was not right to begin with (it goes as it came, and no
/// game takes it)
pub(crate) fn rewrite(frame: &[u8], at: Option<usize>, plate: Option<&[u8]>) -> Option<Vec<u8>> {
    if !intact(frame) {
        return None;
    }
    let d = &frame[FRAME_HDR..];
    let n = d.len();
    let mut out = frame[..FRAME_HDR].to_vec();
    match at {
        Some(at) => {
            let end = at + 1 + *d.get(at)? as usize;
            if end > n - 4 {
                return None;
            }
            out.extend_from_slice(&d[..at]);
            if let Some(p) = plate {
                out.push(p.len() as u8);
                out.extend_from_slice(p);
            }
            out.extend_from_slice(&d[end..n - 4]);
        }
        None => out.extend_from_slice(&d[..n - 4]),
    }
    let flags = u16::from_le_bytes([out[FRAME_HDR + 14], out[FRAME_HDR + 15]]);
    let flags = if plate.is_some_and(|p| !p.is_empty()) { flags } else { flags & !HF_PLATE };
    out[FRAME_HDR + 14..FRAME_HDR + 16].copy_from_slice(&flags.to_le_bytes());
    let len = (out.len() - FRAME_HDR + 4) as u16;
    out[FRAME_HDR + 6..FRAME_HDR + 8].copy_from_slice(&len.to_le_bytes());
    let crc = crc32fast::hash(&out[FRAME_HDR..]);
    out.extend_from_slice(&crc.to_le_bytes());
    Some(out)
}

/// a frame's datagram has the length and CRC it says: a game takes it (one that has not is not judged either)
pub(crate) fn intact(frame: &[u8]) -> bool {
    let Some(d) = frame.get(FRAME_HDR..) else { return false };
    let n = d.len();
    n >= 29 + 4
        && u16::from_le_bytes([d[6], d[7]]) as usize == n
        && u32::from_le_bytes([d[n - 4], d[n - 3], d[n - 2], d[n - 1]]) == crc32fast::hash(&d[..n - 4])
}

/// What the switch keeps of a player's plates (lib.rs Peer)
#[derive(Default)]
pub(crate) struct Seen {
    /// the plate that last went to the others, and when (mono ms)
    last: Option<(Box<[u8]>, i64)>,
    /// the last plate taken out as a slur (its line is written once)
    refused: Option<Box<[u8]>>,
}

/// What to do with a plate HELLO
#[derive(Debug, PartialEq)]
pub(crate) enum Verdict {
    /// on as it is
    Go,
    /// on, a new plate: a `[plate]` line
    New,
    /// on with the player's last plate instead (a change too soon)
    Last(Box<[u8]>),
    /// on without its plate: it reads as a slur, a `[plate]` line (the first time)
    Slur { tell: bool },
}

/// The verdict on `plate` (checked by `hello_plate`) from the player whose plates `seen` keeps, at `now` (mono ms)
pub(crate) fn judge(seen: &Mutex<Seen>, plate: &[u8], now: i64) -> Verdict {
    let mut s = seen.lock();
    if nfsu2_names::blocked_name(std::str::from_utf8(plate).unwrap_or("")) {
        let tell = s.refused.as_deref() != Some(plate);
        s.refused = Some(plate.into());
        return Verdict::Slur { tell };
    }
    match &s.last {
        Some((p, _)) if **p == *plate => Verdict::Go,
        Some((p, at)) if now - at < EVERY_MS => Verdict::Last(p.clone()),
        _ => {
            s.last = Some((plate.into(), now));
            Verdict::New
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// a switch frame with a HELLO of core 17 from "Ann", its plate `plate` (none: no plate field at all), the header's
    /// `flags`, length and CRC right
    pub(crate) fn frame(plate: &[u8], flags: u16) -> Vec<u8> {
        let mut f = vec![0u8; 16];
        f[0] = crate::KIND_UDP;
        let mut d = vec![0u8; 29];
        d[..4].copy_from_slice(b"N2FR");
        d[4] = 1;
        d[5] = 1;
        d[14..16].copy_from_slice(&flags.to_le_bytes());
        d[16] = 17;
        d[28] = 3;
        d.extend_from_slice(b"Ann");
        d.extend_from_slice(&[0, 0, 0, 0, 0, 0, 0, 0]);
        if flags & HF_PLATE != 0 || !plate.is_empty() {
            d.push(plate.len() as u8);
            d.extend_from_slice(plate);
        }
        let n = (d.len() + 4) as u16;
        d[6..8].copy_from_slice(&n.to_le_bytes());
        let crc = crc32fast::hash(&d);
        d.extend_from_slice(&crc.to_le_bytes());
        f.extend_from_slice(&d);
        f
    }

    #[test]
    fn capitals_and_digits_are_a_plate_anything_else_is_broken() {
        for p in [&b"A"[..], b"AB123456", b"0", b"ZZZZZZZZ"] {
            assert!(
                matches!(hello_plate(&frame(p, HF_CREW | HF_PLATE)), Some(Found::Plate { plate, name: b"Ann", .. }) if plate == p),
                "{p:?}"
            );
        }
        for p in [&b""[..], b"123456789", b"lower", b"A B", b"A\0B", b"A-B", "\u{c9}".as_bytes()] {
            assert_eq!(hello_plate(&frame(p, HF_CREW | HF_PLATE)), Some(Found::Broken), "{p:?}");
        }
        // without the flags: not a plate HELLO, or a broken one
        assert_eq!(hello_plate(&frame(b"", HF_CREW)), None);
        assert_eq!(hello_plate(&frame(b"AB12", HF_PLATE)), Some(Found::Broken));
    }

    #[test]
    fn a_hello_cut_short_is_broken() {
        let f = frame(b"ABCDEFGH", HF_CREW | HF_PLATE);
        for n in 16 + 29 + 4..f.len() {
            assert!(!matches!(hello_plate(&f[..n]), Some(Found::Plate { plate: b"ABCDEFGH", .. })), "{n}");
        }
    }

    #[test]
    fn a_rewritten_hello_is_the_one_a_game_would_send_with_that_plate_or_none() {
        let f = frame(b"N1GGER", HF_CREW | HF_PLATE);
        let Some(Found::Plate { at, .. }) = hello_plate(&f) else { panic!("a plate HELLO") };
        // another plate in its place, or none: the HELLO the game makes with it, byte for byte
        assert_eq!(rewrite(&f, Some(at), Some(b"OLD1")).unwrap(), frame(b"OLD1", HF_CREW | HF_PLATE));
        let none = rewrite(&f, Some(at), None).unwrap();
        assert_eq!(none, frame(b"", HF_CREW));
        assert_eq!(hello_plate(&none), None, "no plate HELLO any more");
        // a broken one: the flag off, its bytes left after the rest (a game reads past them: none)
        let b = frame(b"A B", HF_CREW | HF_PLATE);
        let r = rewrite(&b, None, None).unwrap();
        assert_eq!(u16::from_le_bytes([r[16 + 14], r[16 + 15]]), HF_CREW);
        assert_eq!(r.len(), b.len());
        assert_eq!(hello_plate(&r), None);
        // a frame whose CRC or length is not right goes as it came
        let mut bad = f.clone();
        *bad.last_mut().unwrap() ^= 1;
        assert_eq!(rewrite(&bad, Some(at), None), None);
        let mut short = f.clone();
        short.truncate(f.len() - 1);
        assert_eq!(rewrite(&short, Some(at), None), None);
    }

    #[test]
    fn slurs_are_taken_out_and_told_once_changes_wait_ten_seconds() {
        let seen = Mutex::new(Seen::default());
        assert_eq!(judge(&seen, b"N1GGER", 0), Verdict::Slur { tell: true });
        assert_eq!(judge(&seen, b"N1GGER", 100), Verdict::Slur { tell: false });
        assert_eq!(judge(&seen, b"1488", 200), Verdict::Slur { tell: true });
        assert_eq!(judge(&seen, b"ABC123", 300), Verdict::New);
        assert_eq!(judge(&seen, b"ABC123", 400), Verdict::Go);
        assert_eq!(judge(&seen, b"XYZ9", 300 + EVERY_MS - 1), Verdict::Last(b"ABC123"[..].into()));
        assert_eq!(judge(&seen, b"ABC123", 300 + EVERY_MS - 1), Verdict::Go);
        assert_eq!(judge(&seen, b"XYZ9", 300 + EVERY_MS), Verdict::New);
    }
}
