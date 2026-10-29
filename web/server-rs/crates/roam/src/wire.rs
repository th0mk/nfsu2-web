//! The free-roam wire format (runtime/roam_core.h): a 16-byte header ("N2FR", version 1, type, length, sender id,
//! sequence, flags; little-endian), the payload, then the CRC-32 (zlib's) of everything before it.  And the switch's
//! frame around a datagram (nfsu2-lan): `[0]` kind, `[4..8]` source vip, `[8..12]` destination vip, `[12..16]` ports.

use crate::{PeerEntry, RoamMsg};

/// the switch's frame kinds (nfsu2-lan): a UDP datagram
pub(crate) const KIND_UDP: u8 = 1;
/// the switch's header in front of a datagram
pub(crate) const F_DATA: usize = 16;
pub(crate) const MSG_HELLO: u8 = 1;
pub(crate) const MSG_PEERS: u8 = 2;
pub(crate) const MSG_SNAP: u8 = 3;
pub(crate) const MSG_BYE: u8 = 8;
pub(crate) const MSG_MAPREQ: u8 = 10;
pub(crate) const MSG_MAP: u8 = 11;
pub(crate) const MSG_LOBBY: u8 = 12;
pub(crate) const MSG_LOBBIES: u8 = 13;
/// a player's report of a multiplayer race (roam_core.h RC_MSG_RACEREP): only to the hub, which logs it
pub(crate) const MSG_RACEREP: u8 = 14;
/// its payload's bytes (roam_core.h RC_RACEREP_BYTES) and its car name's (RC_RACEREP_NAME)
const RACEREP_BYTES: usize = 64;
const RACEREP_NAME: usize = 16;
/// the race's results as the reporter's game showed them (roam_core.h RC_MSG_RACERES): only to the hub, which compares
/// the players' games' results for a racer far ahead of the others
pub(crate) const MSG_RACERES: u8 = 16;
/// its head and each row (roam_core.h RC_RACERES_HEAD, RC_RACERES_ROW); a row's flags (RC_RS_FINISHED, RC_RS_OURS)
const RACERES_HEAD: usize = 12;
const RACERES_ROW: usize = 20;
pub(crate) const RS_FINISHED: u8 = 0x01;
pub(crate) const RS_OURS: u8 = 0x02;
/// HELLO's flag: this player takes race lobbies (roam_core.h RC_HF_LOBBY)
pub(crate) const HF_LOBBY: u16 = 0x0080;
/// HELLO's flag: this player reads crews (roam_core.h RC_HF_CREW), and its HELLO may end with its crews, after the
/// ids it hears: a count (at most CREWS), then that many hashes (u32 LE, rc_room_hash of each crew's word, never 0);
/// whatever comes after them is skipped (room for what a newer core adds)
pub(crate) const HF_CREW: u16 = 0x0200;
/// the most crews a player is in (roam_core.h RC_CREW_MAX: the start page's four words)
pub(crate) const CREWS: usize = 4;
/// HELLO's flag: this player reads the crew marks of the hub's PEERS (roam_core.h RC_HF_HUBCREW, core 8): HF_CREW on a
/// PEERS says the list ends in them (`write_peers`).  A core before it counts the longer list as bad, so it gets none.
pub(crate) const HF_HUBCREW: u16 = 0x0400;
/// the most ids a HELLO names (roam_core.h RC_MAX_PEERS: its decoder takes no more)
const SEEN_MAX: usize = 32;
/// a LOBBY's ops (roam_core.h RC_LOBBY_*) the hub keeps: the host's advert and its end, and a player's ask to join
pub(crate) const LOBBY_ADVERT: u8 = 1;
/// a player's ask to join (to its host, and a copy to the hub: the hub links the two first)
pub(crate) const LOBBY_JOIN: u8 = 2;
pub(crate) const LOBBY_CLOSE: u8 = 5;
/// a LOBBIES entry: x, y (i16 LE, m), race (u32 LE), players, most players, state (u8); at most LOBBIES_MAX of them
pub(crate) const LOBBIES_ENTRY: usize = 11;
pub(crate) const LOBBIES_MAX: usize = 96;
/// the most players a lobby takes (roam_core.h RC_LOBBY_MAX)
pub(crate) const LOBBY_PLAYERS: usize = 6;
/// a MAP entry: x, y (i16 LE, m), heading (u8, 256 a turn); at most MAP_PER of them a datagram (roam_core.h)
pub(crate) const MAP_ENTRY: usize = 5;
pub(crate) const MAP_PER: usize = 230;
pub(crate) const HDR: usize = 16;
pub(crate) const CRC: usize = 4;

/// What the hub reads of a datagram, the name borrowed (a HELLO comes every second from every player: its name is
/// compared, not copied).
#[derive(Clone, Copy)]
pub(crate) enum Msg<'a> {
    Hello {
        sender: u32,
        core: u16,
        status: u8,
        room: u32,
        name: &'a [u8],
        /// its flags say it takes race lobbies (HF_LOBBY)
        lobbies: bool,
        /// its flags say it reads the crew marks of the hub's PEERS (HF_HUBCREW)
        marks: bool,
        /// its crews as it sent them (HF_CREW), 0 after them: none for a HELLO without the flag, or with the flag
        /// and nothing after its ids
        crews: [u32; CREWS],
    },
    /// v: the newest state's velocity, 0 where roam_core's decoder would not take it (not finite, 200 m/s or more);
    /// heading: where its car points, 256 a turn (the game's atan2 of the forward row, roam_game.c); t_us: its time on
    /// the sender's clock (µs, wraps), which counts on while the clock epoch `clock` stays
    Snap {
        sender: u32,
        status: u8,
        p: [f64; 3],
        v: [f64; 3],
        heading: u8,
        t_us: u32,
        clock: u8,
    },
    /// the world map's question (roam_core.h RC_MSG_MAPREQ): req is echoed in the answer
    MapReq {
        req: u16,
    },
    /// a race lobby (roam_core.h RC_MSG_LOBBY), checked as roam_core.c's lobby_ok: what the hub keeps of one sent to
    /// it (to 0)
    Lobby {
        op: u8,
        to: u32,
        lobby: u32,
        race: u32,
        x: i16,
        y: i16,
        n: u8,
        max: u8,
        state: u8,
        /// its players (n of them, the host first), 0 after them
        ids: [u32; LOBBY_PLAYERS],
    },
    /// a player's report of a race it raced (roam_core.h rc_racerep), checked as roam_core.c's racerep_ok
    RaceRep(RaceRep<'a>),
    /// the race's results as its game showed them (roam_core.h rc_raceres), checked as roam_core.c's raceres_ok
    RaceRes(RaceRes),
    Bye,
    Other,
}

/// roam_core.h rc_racerep: the race, how the reporter did, its car as the game rated it
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct RaceRep<'a> {
    pub lobby: u32,
    pub race: u32,
    /// the network race's type: 0 circuit, 1 sprint, 2 drag, 3 drift, 4 URL, 10 Street X
    pub mode: u8,
    pub laps: u8,
    pub track: u16,
    /// 1 .. players; 0: not finished
    pub place: u8,
    pub players: u8,
    pub host: bool,
    /// left with its pause menu's Quit
    pub quit: bool,
    pub car_type: u8,
    /// 0 fastest .. 3; -1 not rated
    pub class: i8,
    pub rating: f64,
    /// the average rating of the race's cars
    pub field: f64,
    /// its engine power times this in the race
    pub factor: f64,
    pub kw: f64,
    pub kg: f64,
    pub kmh: f64,
    /// s, 0: none
    pub time: f64,
    /// the car type's name, printable ASCII
    pub name: &'a [u8],
}

/// a RACEREP's payload (at o, RACEREP_BYTES long), or None where roam_core.c's decoder would not take it
fn racerep(d: &[u8], o: usize) -> Option<RaceRep<'_>> {
    let f = |k: usize| f32_le(d, o + k);
    let (lobby, race, players, place, flags, class) = (u32_le(d, o), u32_le(d, o + 4), d[o + 13], d[o + 12], d[o + 14], d[o + 16] as i8);
    let nums = [f(20), f(24), f(28), f(32), f(36), f(40), f(44)];
    let lim = [(0.0, 5000.0), (0.0, 5000.0), (0.01, 100.0), (0.0, 10000.0), (0.0, 50000.0), (0.0, 2000.0), (0.0, 100000.0)];
    if lobby == 0
        || race == 0
        || !(1..=LOBBY_PLAYERS as u8).contains(&players)
        || place > players
        || !(-1..=3).contains(&class)
        || flags & !3 != 0
    {
        return None;
    }
    // (the decoder's float ranges: f32 bounds compared as f32, the same as f64 for these values)
    if !nums.iter().zip(lim).all(|(&x, (lo, hi))| x.is_finite() && x >= lo as f32 as f64 && x <= hi as f32 as f64) {
        return None;
    }
    let raw = &d[o + 48..o + 48 + RACEREP_NAME];
    let name = &raw[..raw.iter().position(|&c| c == 0).unwrap_or(RACEREP_NAME)];
    if !name.iter().all(|c| (0x20..=0x7e).contains(c)) {
        return None;
    }
    Some(RaceRep {
        lobby,
        race,
        mode: d[o + 8],
        laps: d[o + 9],
        track: u16_le(d, o + 10),
        place,
        players,
        host: flags & 1 != 0,
        quit: flags & 2 != 0,
        car_type: d[o + 15],
        class,
        rating: nums[0],
        field: nums[1],
        factor: nums[2],
        kw: nums[3],
        kg: nums[4],
        kmh: nums[5],
        time: nums[6],
        name,
    })
}

/// roam_core.h rc_raceres: the race, and its results' rows in their order
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct RaceRes {
    pub lobby: u32,
    pub race: u32,
    pub mode: u8,
    pub laps: u8,
    pub n: usize,
    pub rows: [ResRow; LOBBY_PLAYERS],
}

/// one racer's row: its player's id (0: one the reporter could not name), RS_* flags, its time (s, a finisher's), how
/// far it came (laps) and the way it drove (m: its average speed for its time, as the reporter's game counted it)
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct ResRow {
    pub id: u32,
    pub flags: u8,
    pub time: f64,
    pub done: f64,
    pub dist: f64,
}

/// a RACERES's payload (at o, `len` bytes), or None where roam_core.c's decoder would not take it
fn raceres(d: &[u8], o: usize, len: usize) -> Option<RaceRes> {
    if len < RACERES_HEAD {
        return None;
    }
    let n = d[o + 10] as usize;
    if !(1..=LOBBY_PLAYERS).contains(&n) || len != RACERES_HEAD + RACERES_ROW * n {
        return None;
    }
    let (lobby, race) = (u32_le(d, o), u32_le(d, o + 4));
    if lobby == 0 || race == 0 {
        return None;
    }
    let mut rows = [ResRow::default(); LOBBY_PLAYERS];
    for (k, row) in rows.iter_mut().enumerate().take(n) {
        let at = o + RACERES_HEAD + RACERES_ROW * k;
        let (time, done, dist) = (f32_le(d, at + 8), f32_le(d, at + 12), f32_le(d, at + 16));
        let flags = d[at + 4];
        let fits = |x: f64, max: f64| x.is_finite() && (0.0..=max).contains(&x);
        if flags & !(RS_FINISHED | RS_OURS) != 0 || !fits(time, 100_000.0) || !fits(done, 1000.0) || !fits(dist, 1e6) {
            return None;
        }
        *row = ResRow { id: u32_le(d, at), flags, time, done, dist };
    }
    if rows[..n].iter().filter(|r| r.flags & RS_OURS != 0).count() > 1 {
        return None;
    }
    Some(RaceRes { lobby, race, mode: d[o + 8], laps: d[o + 9], n, rows })
}

fn u16_le(d: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([d[o], d[o + 1]])
}

fn u32_le(d: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}

fn f32_le(d: &[u8], o: usize) -> f64 {
    f32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]]) as f64
}

/// a state's velocity as roam_core.c's get_state takes it (in float: finite, |v|^2 at most 200^2), else 0
fn velocity(d: &[u8], o: usize) -> [f64; 3] {
    let v = [0, 4, 8].map(|k| f32::from_le_bytes([d[o + k], d[o + k + 1], d[o + k + 2], d[o + k + 3]]));
    if v.iter().all(|x| x.is_finite()) && v[0] * v[0] + v[1] * v[1] + v[2] * v[2] <= 200.0 * 200.0 { v.map(f64::from) } else { [0.0; 3] }
}

/// where a state's car points, 256 a turn: the forward row of its quaternion (i16 x y z w at o, roam_core.c rc_q2m),
/// atan2(f[1], f[0]) as roam_game.c turns it into the car's heading; 0 for a quaternion of almost nothing
fn heading(d: &[u8], o: usize) -> u8 {
    let q = [0, 2, 4, 6].map(|k| i16::from_le_bytes([d[o + k], d[o + k + 1]]) as f64 / 32767.0);
    let (f0, f1) = (1.0 - 2.0 * (q[1] * q[1] + q[2] * q[2]), 2.0 * (q[0] * q[1] + q[2] * q[3]));
    if q.iter().map(|x| x * x).sum::<f64>() < 0.25 {
        return 0;
    }
    ((f1.atan2(f0) * 128.0 / std::f64::consts::PI + 0.5).floor() as i64 & 255) as u8
}

/// One free-roam datagram, or None: the fields the hub needs (HELLO, SNAP, BYE), checked as roam_core.c's decoder
/// does.  Every index is behind a length check above it.
pub(crate) fn decode(d: &[u8]) -> Option<(u8, u32, Msg<'_>)> {
    if d.len() < HDR + CRC || d.len() > 1200 || &d[..4] != b"N2FR" || d[4] != 1 {
        return None;
    }
    let end = d.len() - CRC;
    if u16_le(d, 6) as usize != d.len() || crc32fast::hash(&d[..end]) != u32_le(d, end) {
        return None;
    }
    let (kind, sender) = (d[5], u32_le(d, 8));
    if sender == 0 {
        return None;
    }
    let m = match kind {
        MSG_HELLO => {
            if end < 29 {
                return None;
            }
            let (core, status, room, nl) = (u16_le(d, 16), d[19], u32_le(d, 24), d[28] as usize);
            if nl > 24 || end < 29 + nl + 1 || status > 5 {
                return None;
            }
            let name = &d[29..29 + nl];
            // /^[\x20-\x7e]*$/
            if !name.iter().all(|c| (0x20..=0x7e).contains(c)) {
                return None;
            }
            let flags = u16_le(d, 14);
            let mut crews = [0u32; CREWS];
            // the crews after the ids it hears (n_seen, then n_seen ids), only with the flag: without it the bytes
            // after the name's are not read, as before the flag (a page of before sends what it likes there)
            if flags & HF_CREW != 0 {
                let seen = d[29 + nl] as usize;
                let at = 29 + nl + 1 + 4 * seen;
                if seen > SEEN_MAX || at > end {
                    return None;
                }
                if at < end {
                    let n = d[at] as usize;
                    if n > CREWS || at + 1 + 4 * n > end {
                        return None;
                    }
                    for (k, c) in crews.iter_mut().enumerate().take(n) {
                        *c = u32_le(d, at + 1 + 4 * k);
                    }
                    if crews[..n].contains(&0) {
                        return None;
                    }
                }
            }
            Msg::Hello { sender, core, status, room, name, lobbies: flags & HF_LOBBY != 0, marks: flags & HF_HUBCREW != 0, crews }
        }
        MSG_SNAP => {
            if end < 22 + 52 {
                return None;
            }
            let (status, n) = (d[16], d[20] as usize);
            if !(1..=3).contains(&n) || status > 5 || end != 22 + 52 * n {
                return None;
            }
            // the newest state's position
            let p = [f32_le(d, 26), f32_le(d, 30), f32_le(d, 34)];
            if !p.iter().all(|x| x.is_finite()) || p[0].abs() > 20000.0 || p[1].abs() > 20000.0 || p[2].abs() > 2000.0 {
                return None;
            }
            Msg::Snap { sender, status, p, v: velocity(d, 38), heading: heading(d, 50), t_us: u32_le(d, 22), clock: d[21] }
        }
        MSG_MAPREQ if end == HDR + 2 => Msg::MapReq { req: u16_le(d, 16) },
        MSG_LOBBY => {
            // op arg state max, to, lobby, race, x y, t, n, a reserved byte, then n ids (roam_core.c rc_encode)
            if end < HDR + 24 {
                return None;
            }
            let (op, arg, state, max, n) = (d[16], d[17], d[18], d[19], d[38] as usize);
            if !(1..6).contains(&op) || arg >= 5 || state >= 3 || !(2..=6).contains(&max) || n > max as usize || end != HDR + 24 + 4 * n {
                return None;
            }
            let mut ids = [0u32; LOBBY_PLAYERS];
            for (i, v) in ids.iter_mut().enumerate().take(n) {
                *v = u32_le(d, HDR + 24 + 4 * i);
            }
            let (lobby, race) = (u32_le(d, 24), u32_le(d, 28));
            if lobby == 0 || race == 0 || ids[..n].iter().enumerate().any(|(i, &v)| v == 0 || ids[..i].contains(&v)) {
                return None;
            }
            Msg::Lobby { op, to: u32_le(d, 20), lobby, race, x: u16_le(d, 32) as i16, y: u16_le(d, 34) as i16, n: n as u8, max, state, ids }
        }
        MSG_RACEREP if end == HDR + RACEREP_BYTES => Msg::RaceRep(racerep(d, HDR)?),
        MSG_RACERES => Msg::RaceRes(raceres(d, HDR, end - HDR)?),
        MSG_BYE => Msg::Bye,
        _ => Msg::Other,
    };
    Some((kind, sender, m))
}

/// One free-roam datagram, or None: what `decode` reads of it, the name its own
pub fn decode_roam(d: &[u8]) -> Option<RoamMsg> {
    let (kind, sender, m) = decode(d)?;
    Some(match m {
        // the name is printable ASCII (checked): latin1 is the bytes as they are
        Msg::Hello { sender, core, status, room, name, crews, .. } => RoamMsg::Hello {
            sender,
            core,
            status,
            room,
            name: name.iter().map(|&c| c as char).collect(),
            crews: crews.iter().copied().take_while(|&c| c != 0).collect(),
        },
        Msg::Snap { sender, status, p, .. } => RoamMsg::Snap { sender, status, p },
        Msg::Bye => RoamMsg::Bye { sender },
        Msg::MapReq { .. } | Msg::Lobby { .. } | Msg::RaceRep(_) | Msg::RaceRes(_) | Msg::Other => RoamMsg::Other { kind, sender },
    })
}

/// the length of a PEERS datagram of n entries
pub(crate) fn peers_len(n: usize) -> usize {
    HDR + 1 + 10 * n + CRC
}

/// the length of a PEERS datagram of n entries that ends in crew marks for a player of k crews (`write_peers`)
pub(crate) fn peers_crew_len(n: usize, k: usize) -> usize {
    peers_len(n) + 1 + 4 * k + n
}

/// Writes a PEERS datagram into `b` (exactly `peers_len(n)` bytes long, or `peers_crew_len(n, mine.len())` with crews):
/// list = [{ vip, port, id }] (at most 32: roam_core's RC_MAX_PEERS; 7 for a core of version 1).  With `mine` (the
/// receiver's crews, at most CREWS, none 0) the list ends in its crew marks (HF_CREW on it): `mine`, then a byte per
/// entry, `marks[i]` bit c: entry i is in crew `mine[c]` too.  So a player learns which of its peers are in its crews
/// without anyone's crews but its own going out.
pub(crate) fn write_peers(
    b: &mut [u8],
    sender: u32,
    seq: u16,
    list: impl ExactSizeIterator<Item = PeerEntry>,
    mine: &[u32],
    marks: &[u8],
) {
    let len = b.len();
    let n = list.len();
    b[..4].copy_from_slice(b"N2FR");
    b[4] = 1;
    b[5] = MSG_PEERS;
    b[6..8].copy_from_slice(&(len as u16).to_le_bytes());
    b[8..12].copy_from_slice(&sender.to_le_bytes());
    b[12..14].copy_from_slice(&seq.to_le_bytes());
    b[14..16].copy_from_slice(&if mine.is_empty() { 0 } else { HF_CREW }.to_le_bytes());
    // the count's low byte: one byte holds every count roam_core reads (RC_MAX_PEERS at most, as above)
    b[16] = n as u8;
    for (i, e) in list.enumerate() {
        let o = 17 + 10 * i;
        b[o..o + 4].copy_from_slice(&e.vip.to_be_bytes());
        b[o + 4..o + 6].copy_from_slice(&e.port.to_le_bytes());
        b[o + 6..o + 10].copy_from_slice(&e.id.to_le_bytes());
    }
    if !mine.is_empty() {
        let mut o = 17 + 10 * n;
        b[o] = mine.len() as u8;
        o += 1;
        for c in mine {
            b[o..o + 4].copy_from_slice(&c.to_le_bytes());
            o += 4;
        }
        b[o..o + n].copy_from_slice(&marks[..n]);
    }
    let crc = crc32fast::hash(&b[..len - CRC]);
    b[len - CRC..].copy_from_slice(&crc.to_le_bytes());
}

/// A PEERS datagram: `write_peers` into a buffer of its own
pub fn encode_peers(sender: u32, seq: u16, list: &[PeerEntry]) -> Vec<u8> {
    encode_peers_crews(sender, seq, list, &[], &[])
}

/// A PEERS datagram with crew marks (`write_peers`; none with `mine` empty)
pub fn encode_peers_crews(sender: u32, seq: u16, list: &[PeerEntry], mine: &[u32], marks: &[u8]) -> Vec<u8> {
    let mut b = vec![0u8; if mine.is_empty() { peers_len(list.len()) } else { peers_crew_len(list.len(), mine.len()) }];
    write_peers(&mut b, sender, seq, list.iter().copied(), mine, marks);
    b
}

/// the length of a LOBBIES datagram of n entries
pub(crate) fn lobbies_len(n: usize) -> usize {
    HDR + 3 + LOBBIES_ENTRY * n + CRC
}

/// One LOBBIES datagram, the open race lobbies of the asker's room with the answer to its map question `req`
/// (roam_core.h RC_MSG_LOBBIES) into `b` (exactly `lobbies_len(n)` bytes long): (x, y, race, players, most, state)
pub(crate) fn write_lobbies(b: &mut [u8], sender: u32, seq: u16, req: u16, list: &[(i16, i16, u32, u8, u8, u8)]) {
    let len = b.len();
    b[..4].copy_from_slice(b"N2FR");
    b[4] = 1;
    b[5] = MSG_LOBBIES;
    b[6..8].copy_from_slice(&(len as u16).to_le_bytes());
    b[8..12].copy_from_slice(&sender.to_le_bytes());
    b[12..14].copy_from_slice(&seq.to_le_bytes());
    b[14..16].copy_from_slice(&[0, 0]);
    b[16..18].copy_from_slice(&req.to_le_bytes());
    b[18] = list.len() as u8;
    for (i, &(x, y, race, n, max, state)) in list.iter().enumerate() {
        let o = 19 + LOBBIES_ENTRY * i;
        b[o..o + 2].copy_from_slice(&x.to_le_bytes());
        b[o + 2..o + 4].copy_from_slice(&y.to_le_bytes());
        b[o + 4..o + 8].copy_from_slice(&race.to_le_bytes());
        b[o + 8] = n;
        b[o + 9] = max;
        b[o + 10] = state;
    }
    let crc = crc32fast::hash(&b[..len - CRC]);
    b[len - CRC..].copy_from_slice(&crc.to_le_bytes());
}

/// the length of a MAP datagram of n entries
pub(crate) fn map_len(n: usize) -> usize {
    HDR + 7 + MAP_ENTRY * n + CRC
}

/// One MAP datagram, part `part` of `nparts` of the answer to question `req` (roam_core.h RC_MSG_MAP) into `b`
/// (exactly `map_len(n)` bytes long): `total` players past the asker and its peers, these n of them (x, y, heading).
#[allow(clippy::too_many_arguments)]
pub(crate) fn write_map(b: &mut [u8], sender: u32, seq: u16, req: u16, part: u8, nparts: u8, total: u16, list: &[(i16, i16, u8)]) {
    let len = b.len();
    b[..4].copy_from_slice(b"N2FR");
    b[4] = 1;
    b[5] = MSG_MAP;
    b[6..8].copy_from_slice(&(len as u16).to_le_bytes());
    b[8..12].copy_from_slice(&sender.to_le_bytes());
    b[12..14].copy_from_slice(&seq.to_le_bytes());
    b[14..16].copy_from_slice(&[0, 0]);
    b[16..18].copy_from_slice(&req.to_le_bytes());
    b[18] = part;
    b[19] = nparts;
    b[20..22].copy_from_slice(&total.to_le_bytes());
    b[22] = list.len() as u8;
    for (i, &(x, y, h)) in list.iter().enumerate() {
        let o = 23 + MAP_ENTRY * i;
        b[o..o + 2].copy_from_slice(&x.to_le_bytes());
        b[o + 2..o + 4].copy_from_slice(&y.to_le_bytes());
        b[o + 4] = h;
    }
    let crc = crc32fast::hash(&b[..len - CRC]);
    b[len - CRC..].copy_from_slice(&crc.to_le_bytes());
}
