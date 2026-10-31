//! What the pages were sent, by kind: the [players] count of where the bytes go.  Each entry of a compact bundle
//! (compact.rs) counts its bytes as it went, under its datagram's kind: free roam's by their type (runtime/roam_core.h
//! RC_MSG_*), LAN play's UDP (the game's own races, Online's and free roam's lobby races) and the rest; each message
//! of a host (the free-roam hub's lists and map, the Online server's segments: `send_to`, a WebSocket message each,
//! never in a bundle) counts as its whole frame.  On nfsu2.xyz (2026-10-30, 5,800 players) the entries other than car
//! states carried more bytes than the car states, 104 bytes each on average: this says which they were.

use crate::compact::ROAM_OK;
use crate::{BROADCAST, KIND_UDP};

/// runtime/roam_core.h RC_MSG_*: a free-roam datagram's type (0: one this does not know)
const ROAM_TYPES: [&str; 18] = [
    "?", "hello", "peers", "car state", "car look", "car look ask", "ping", "pong", "bye", "duel", "map ask", "map",
    "lobby", "lobbies", "race report", "say", "race results", "map delta",
];
const R: usize = ROAM_TYPES.len();
/// a free-roam datagram whose magic, length or CRC did not check out (or another kind 1 on free roam's ports)
const ROAM_BAD: usize = R;
const UDP_OTHER: usize = R + 1;
/// LAN play's UDP (KIND_LAN_UDP), to the page's own address, to all
const LAN_UDP: usize = R + 2;
const LAN_BCAST: usize = R + 3;
const OTHER_KIND: usize = R + 4;
/// the hub's messages, by free roam's types (R of them)
const HUB: usize = R + 5;
const ONLINE: usize = HUB + R;
const OTHER_HOST: usize = ONLINE + 1;
/// the kinds
pub const KINDS: usize = OTHER_HOST + 1;
/// runtime/web/net_proto.h: LAN play's UDP
const KIND_LAN_UDP: u8 = 3;
/// runtime/roam_core.h RC_DEFAULT_PORT
const ROAM_PORT: u16 = 47210;

/// a kind's name in the count
pub fn name(i: usize) -> String {
    match i {
        i if i < R => ROAM_TYPES[i].to_string(),
        ROAM_BAD => "free roam's port, unchecked".into(),
        UDP_OTHER => "UDP, other ports".into(),
        LAN_UDP => "LAN UDP".into(),
        LAN_BCAST => "LAN UDP broadcast".into(),
        OTHER_KIND => "other frames".into(),
        i if i < ONLINE => format!("hub {}", ROAM_TYPES[i - HUB]),
        ONLINE => "Online server".into(),
        _ => "other host".into(),
    }
}

/// a free-roam datagram's type at [16 + 5] of its frame, as a kind (0 for one past those known)
fn roam_type(data: &[u8]) -> usize {
    let t = data[16 + 5] as usize;
    if t < R { t } else { 0 }
}

/// nfsu2-roam's ROAM_HUB_VIP: the free-roam hub's address, whose lists, map and lobbies go in bundles too
const HUB_VIP: u32 = crate::BASE | 0xFD;

/// the kind of a datagram (the switch's copy: byte 1 says whether free roam's checked out) that went from `src` to
/// `dst` in a compact bundle (the hub's by their type, as its messages of their own were: `of_host`)
pub(crate) fn of_entry(data: &[u8], src: u32, dst: u32) -> usize {
    if data.len() < 16 {
        return OTHER_KIND;
    }
    match data[0] {
        KIND_UDP if src == HUB_VIP && data[1] == ROAM_OK && data.len() > 16 + 5 => HUB + roam_type(data),
        KIND_UDP if data[1] == ROAM_OK && data.len() > 16 + 5 => roam_type(data),
        KIND_UDP if u16::from_be_bytes([data[14], data[15]]) == ROAM_PORT => ROAM_BAD,
        KIND_UDP => UDP_OTHER,
        KIND_LAN_UDP if dst == BROADCAST => LAN_BCAST,
        KIND_LAN_UDP => LAN_UDP,
        _ => OTHER_KIND,
    }
}

/// the kind of a host's message (its source at [4..8])
pub(crate) fn of_host(data: &[u8], roam: Option<u32>, online: Option<u32>) -> usize {
    let src = (data.len() >= 8).then(|| u32::from_be_bytes([data[4], data[5], data[6], data[7]]));
    if src.is_some() && src == roam {
        if data.len() > 16 + 5 && data[0] == KIND_UDP && &data[16..20] == b"N2FR" { HUB + roam_type(data) } else { HUB }
    } else if src.is_some() && src == online {
        ONLINE
    } else {
        OTHER_HOST
    }
}

/// The count's line: the kinds by their bytes since `was`, the largest first, those of at least 0.5 % of all (and
/// how many fell under that).  `now` and `was`: bytes of each kind, then entries of each.
pub fn line(now: &[u64], was: &[u64], secs: f64) -> Option<String> {
    let d = |i: usize| now[i].saturating_sub(was.get(i).copied().unwrap_or(0));
    let total: u64 = (0..KINDS).map(d).sum();
    if total == 0 {
        return None;
    }
    let mut ks: Vec<usize> = (0..KINDS).filter(|&i| d(i) > 0).collect();
    ks.sort_by_key(|&i| std::cmp::Reverse(d(i)));
    let secs = secs.max(1.0);
    let shown: Vec<String> = ks
        .iter()
        .filter(|&&i| d(i) * 200 >= total)
        .map(|&i| {
            let (b, n) = (d(i), d(KINDS + i).max(1));
            format!(
                "{} {:.0} % ({:.1} Mbit/s, {:.0}/s, {:.1} B each)",
                name(i),
                b as f64 * 100.0 / total as f64,
                b as f64 * 8.0 / secs / 1e6,
                n as f64 / secs,
                b as f64 / n as f64
            )
        })
        .collect();
    let rest = ks.len() - shown.len();
    Some(format!(
        "{:.1} Mbit/s: {}{}",
        total as f64 * 8.0 / secs / 1e6,
        shown.join(", "),
        if rest > 0 { format!(", {rest} smaller kinds") } else { String::new() }
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::BASE;

    fn frame(kind: u8, b1: u8, src: u32, dport: u16, data: &[u8]) -> Vec<u8> {
        let mut f = vec![kind, b1, 0, 0];
        f.extend_from_slice(&src.to_be_bytes());
        f.extend_from_slice(&(BASE | 9).to_be_bytes());
        f.extend_from_slice(&ROAM_PORT.to_be_bytes());
        f.extend_from_slice(&dport.to_be_bytes());
        f.extend_from_slice(data);
        f
    }

    // every kind has a name, the entries and the hosts' messages land where they are
    #[test]
    fn kinds() {
        let mut roam = b"N2FR\x01\x06".to_vec();
        roam.resize(24, 0);
        assert_eq!(name(of_entry(&frame(1, ROAM_OK, BASE | 2, ROAM_PORT, &roam), BASE | 2, BASE | 9)), "ping");
        assert_eq!(name(of_entry(&frame(1, 0, BASE | 2, ROAM_PORT, &roam), BASE | 2, BASE | 9)), "free roam's port, unchecked");
        assert_eq!(name(of_entry(&frame(1, 0, BASE | 2, 3659, &roam), BASE | 2, BASE | 9)), "UDP, other ports");
        assert_eq!(name(of_entry(&frame(3, 0, BASE | 2, 9999, &roam), BASE | 2, BASE | 9)), "LAN UDP");
        assert_eq!(name(of_entry(&frame(3, 0, BASE | 2, 9999, &roam), BASE | 2, BROADCAST)), "LAN UDP broadcast");
        assert_eq!(name(of_entry(&[3, 0, 0], BASE | 2, BASE | 9)), "other frames");
        let mut peers = roam.clone();
        peers[5] = 2;
        let (hub, online) = (Some(BASE | 0xFD), Some(BASE | 1));
        assert_eq!(name(of_host(&frame(1, 0, BASE | 0xFD, ROAM_PORT, &peers), hub, online)), "hub peers");
        // the hub's list in a bundle: the same kind
        assert_eq!(name(of_entry(&frame(1, ROAM_OK, BASE | 0xFD, ROAM_PORT, &peers), BASE | 0xFD, BASE | 9)), "hub peers");
        assert_eq!(name(of_entry(&frame(1, ROAM_OK, BASE | 2, ROAM_PORT, &peers), BASE | 2, BASE | 9)), "peers");
        peers[5] = 200;
        assert_eq!(name(of_host(&frame(1, 0, BASE | 0xFD, ROAM_PORT, &peers), hub, online)), "hub ?");
        assert_eq!(name(of_host(&frame(4, 0, BASE | 1, 0, &[1; 30]), hub, online)), "Online server");
        assert_eq!(name(of_host(&frame(4, 0, BASE | 7, 0, &[1; 30]), hub, online)), "other host");
        let names: std::collections::HashSet<String> = (0..KINDS).map(name).collect();
        assert_eq!(names.len(), KINDS, "each its own");
    }

    #[test]
    fn the_line() {
        let mut now = vec![0u64; 2 * KINDS];
        now[3] = 30_000_000;
        now[KINDS + 3] = 1_000_000;
        now[LAN_UDP] = 60_000_000;
        now[KINDS + LAN_UDP] = 500_000;
        now[6] = 100;
        now[KINDS + 6] = 5;
        let l = line(&now, &[], 300.0).unwrap();
        assert!(l.starts_with("2.4 Mbit/s: LAN UDP 67 % (1.6 Mbit/s, 1667/s, 120.0 B each), car state 33 %"), "{l}");
        assert!(l.ends_with(", 1 smaller kinds"), "{l}");
        assert_eq!(line(&now, &now, 300.0), None);
    }
}
