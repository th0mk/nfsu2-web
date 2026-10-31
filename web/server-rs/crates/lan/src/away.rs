//! Players in free roam who are not driving: in a race, a shop, the menus or loading (runtime/roam_core.h RC_ST_*, as
//! their own HELLOs and SNAPs say).  Their game draws no other car then, and no one draws theirs, but the hub keeps
//! their links (up to 32 friends, so that the cars are there as soon as they drive again): pings, pongs and HELLOs both
//! ways, and their friends' car states 20 times a second.  On nfsu2.xyz (2026-10-30, 5,800 players) about 2,900 of the
//! 5,060 in free roam were such players, and the bots measured what they cost: a player in a race got 70 % of the
//! messages of one driving (web/tools/roam-crowd.mjs --race), each a message the server and cloudflared pay for (the
//! kernel's work for each message, not its bytes, was most of the CPU).
//!
//! So what their friends send them waits for the slow clock (`LanOptions::away_ms`, half a second: a ping's and a
//! pong's times are moved on by their waits, compact.rs `waited`), and of the car states sent to them they get one in
//! 16 (`SHARE`); the hub's own messages (its lists, the world map) and LAN play's go as they go to anyone, and take
//! what waits along.  The first HELLO or SNAP of theirs that says they drive (or are paused there) ends it: the next
//! tick brings them every friend's car.  Players driving near each other get what they always got.

use crate::lag::SHARE_FULL;

/// of the car states sent to a player who is not driving, the share it gets (as a path far behind gets, lag.rs)
pub(crate) const SHARE: u32 = SHARE_FULL / 16;

/// runtime/roam_core.h RC_DEFAULT_PORT
const ROAM_PORT: u16 = 47210;

/// The free-roam status a frame of a player's own says, when it is a HELLO or a SNAP of free roam (to anyone: its
/// friends, the hub): RC_ST_*.  The CRC is not checked: a player who says the wrong thing gets its own traffic slower
/// or as it is now, no one else's.
pub(crate) fn status(frame: &[u8]) -> Option<u8> {
    if frame.len() < 16 + 20 || frame[0] != crate::KIND_UDP || &frame[16..20] != b"N2FR" || frame[20] != 1 {
        return None;
    }
    if u16::from_be_bytes([frame[14], frame[15]]) != ROAM_PORT {
        return None;
    }
    // after the datagram's 16-byte header: a HELLO's core version (2) and platform (1), then its status; a SNAP's
    // status first
    match frame[21] {
        1 => Some(frame[16 + 19]),
        3 => Some(frame[16 + 16]),
        _ => None,
    }
}

/// a status of a player who is not driving: the menus (0), loading (1), a race (3), a shop (4); not in free roam (2)
/// or paused there (5), and not a number the core does not send (as for anyone)
pub(crate) fn away(status: u8) -> bool {
    matches!(status, 0 | 1 | 3 | 4)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frame(kind: u8, port: u16, body: &[u8]) -> Vec<u8> {
        let mut f = vec![0u8; 16];
        f[0] = crate::KIND_UDP;
        f[12..14].copy_from_slice(&ROAM_PORT.to_be_bytes());
        f[14..16].copy_from_slice(&port.to_be_bytes());
        f.extend_from_slice(b"N2FR");
        f.extend_from_slice(&[1, kind, 0, 0, 1, 2, 3, 4, 0, 1, 0, 0]);
        f.extend_from_slice(body);
        f.extend_from_slice(&[0; 4]);
        f
    }

    #[test]
    fn a_hellos_and_a_snaps_status() {
        // HELLO: core version 8, platform 3, status 4 (a shop)
        assert_eq!(status(&frame(1, ROAM_PORT, &[8, 0, 3, 4, 0, 0, 0, 0])), Some(4));
        // SNAP: status 2 (driving) first
        assert_eq!(status(&frame(3, ROAM_PORT, &[2, 7, 0, 0, 1, 0, 0, 0])), Some(2));
        // a ping, another port, another kind of frame: none
        assert_eq!(status(&frame(6, ROAM_PORT, &[0; 8])), None);
        assert_eq!(status(&frame(1, 9999, &[8, 0, 3, 4, 0, 0, 0, 0])), None);
        let mut lan = frame(1, ROAM_PORT, &[8, 0, 3, 4, 0, 0, 0, 0]);
        lan[0] = crate::KIND_LAN_UDP;
        assert_eq!(status(&lan), None);
        assert_eq!(status(&frame(1, ROAM_PORT, &[])[..30]), None);
        assert!(away(0) && away(1) && away(3) && away(4) && !away(2) && !away(5) && !away(6) && !away(202));
    }
}
