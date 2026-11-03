//! How far behind each player's path is, and the share of free roam's snapshots it gets meanwhile.
//!
//! Between the switch and a player are queues the switch never sees: the kernel's socket buffers, a proxy's
//! (cloudflared's: Linux grows its receive buffers to megabytes), the tunnel to Cloudflare, Cloudflare's own.  When
//! the path takes less than free roam sends the player (32 friends, 20 snapshots a second each), the rest waits
//! there and the player sees the other cars late.  The switch's own queue rules (`roam_queue`) count only what the
//! kernel did not take yet, and it takes megabytes first: nfsu2.xyz, 2026-10-22, 2,950 players, two of the
//! Cloudflare Tunnel's connections on a lossy route, about 600 players had 64 KB to 1 MB waiting at cloudflared
//! (one of them measured 4-26 s behind) while the server was idle.
//!
//! So the switch times each path: a WebSocket ping every `probe_ms` (the browser answers it by itself) goes out
//! behind everything queued before it, and its pong comes back once all of that was read.  The lowest round trip of
//! the last 30-60 s (at most `BASE_MAX_MS`: a path queued from the start must not count as its own base) is the path
//! without queues; what a round trip has above it waited in them.  A path whose round trips stay above that through
//! two whole windows of a cut share is that long without queues of ours (a satellite link, a far mobile one): its
//! lowest round trip is its base from then on, or it never got its share back.  While a player's waits are long, it gets only a
//! share of the snapshots sent to it (RC_MSG_SNAP, where a car is; every other message of free roam goes, so a
//! player keeps its links, a car's record, a race): the share halves for a round trip over `LAG_HIGH_MS` (once for
//! each round trip: only a ping queued after the last cut cuts again), drops to the least at once past
//! `LAG_SEVERE_MS`, and grows by an eighth for each round trip under `LAG_LOW_MS`.  The queues drain, and the
//! player gets the cars as they are now, fewer times a second, instead of all of them seconds late.

use std::cell::Cell;
use std::collections::VecDeque;
use std::sync::atomic::{AtomicU32, Ordering::Relaxed};

/// a player's share of the snapshots sent to it, in 1/SHARE_FULL (`Peer::share`)
pub(crate) const SHARE_FULL: u32 = 1 << 16;
/// the least share: a snapshot in 16 (a car near by still moves about once a second)
pub(crate) const SHARE_MIN: u32 = SHARE_FULL / 16;
/// a round trip this much over the base: halve the share
const LAG_HIGH_MS: f64 = 400.0;
/// this much: the least share at once
const LAG_SEVERE_MS: f64 = 1500.0;
/// under this much: an eighth more
const LAG_LOW_MS: f64 = 150.0;
/// the base is the lowest round trip of this window and the one before
const BASE_WINDOW_MS: f64 = 30_000.0;
/// and at most this (Cloudflare to the far side of the world and back, with room), until a cut share shows the path is
/// longer (`Probe::floor`)
const BASE_MAX_MS: f64 = 400.0;
/// pings waiting for their pong at most (past it the oldest is forgotten: the keepalive cuts such a player anyway)
const MAX_OUT: usize = 64;

/// One player's pings and what their round trips say.
pub(crate) struct Probe {
    /// the pings not answered yet, oldest first: payload, and when it was queued (mono ms)
    out: VecDeque<([u8; 8], f64)>,
    /// the keepalive's last tick: only a pong to a ping queued since says the client is there (as before probes: a
    /// pong to the last keepalive ping by the next tick)
    tick_at: f64,
    /// the last round trip (ms; NaN before the first)
    rtt: f64,
    /// the lowest round trip of this window and of the one before, and when this one and the one before began
    base_now: f64,
    base_prev: f64,
    window_at: f64,
    prev_at: f64,
    /// the most the base counts as (BASE_MAX_MS, or the path's own lowest round trip through two windows of a cut share)
    floor: f64,
    /// when the share was last cut: only a ping queued after it halves it again
    cut_at: f64,
    /// since when the share is under all (infinity: it is all)
    thin_at: f64,
}

impl Probe {
    pub(crate) fn new(now: f64) -> Probe {
        Probe {
            out: VecDeque::new(),
            tick_at: now,
            rtt: f64::NAN,
            base_now: f64::INFINITY,
            base_prev: f64::INFINITY,
            window_at: now,
            prev_at: f64::NEG_INFINITY,
            floor: BASE_MAX_MS,
            cut_at: f64::NEG_INFINITY,
            thin_at: f64::INFINITY,
        }
    }

    /// a new ping's payload (random: only a client that reads can answer it), noted with its time
    pub(crate) fn ping(&mut self, now: f64) -> [u8; 8] {
        let mut data = [0u8; 8];
        nfsu2_util::rand::fill(&mut data);
        if self.out.len() == MAX_OUT {
            self.out.pop_front();
        }
        self.out.push_back((data, now));
        data
    }

    /// the keepalive's tick: from now on only a pong to a ping queued since says the client is there
    pub(crate) fn tick(&mut self, now: f64) {
        self.tick_at = now;
    }

    /// a pong: None when it answers none of the pings waiting (it never counts as a sign of life), else whether it
    /// says the client is there.  Its round trip moves `share`.  A client may answer only the newest of several
    /// pings (RFC 6455 5.5.3), so those before the one answered go.
    pub(crate) fn pong(&mut self, d: &[u8], now: f64, share: &AtomicU32) -> Option<bool> {
        let i = self.out.iter().position(|(p, _)| p[..] == *d)?;
        let sent = self.out[i].1;
        self.out.drain(..=i);
        let rtt = now - sent;
        self.rtt = rtt;
        if now - self.window_at >= BASE_WINDOW_MS {
            self.base_prev = self.base_now;
            self.base_now = f64::INFINITY;
            self.prev_at = self.window_at;
            self.window_at = now;
        }
        self.base_now = self.base_now.min(rtt);
        let base = self.base_now.min(self.base_prev);
        // both windows of the base under a cut share: what is left above BASE_MAX_MS is the path, not a queue of ours
        if self.thin_at <= self.prev_at {
            self.floor = self.floor.max(base);
        }
        let waited = rtt - base.min(self.floor);
        let s = share.load(Relaxed);
        let next = if waited > LAG_SEVERE_MS {
            SHARE_MIN
        } else if waited > LAG_HIGH_MS && sent > self.cut_at {
            (s / 2).max(SHARE_MIN)
        } else if waited < LAG_LOW_MS {
            (s + SHARE_FULL / 8).min(SHARE_FULL)
        } else {
            s
        };
        if next < s {
            self.cut_at = now;
        }
        if next == SHARE_FULL {
            self.thin_at = f64::INFINITY;
        } else if self.thin_at == f64::INFINITY {
            self.thin_at = now;
        }
        share.store(next, Relaxed);
        Some(sent >= self.tick_at)
    }

    /// the path's round trip without queues (ms): its lowest of the last 30-60 s, as the base above (not capped at
    /// `floor`); None before its first pong (paths.rs, the server's counts)
    pub(crate) fn base(&self) -> Option<f64> {
        let b = self.base_now.min(self.base_prev);
        b.is_finite().then_some(b)
    }

    /// how far behind the path is now (ms): the last round trip, or longer while a ping waits longer than that
    pub(crate) fn lag(&self, now: f64) -> f64 {
        let waiting = self.out.front().map_or(0.0, |(_, sent)| now - sent);
        if self.rtt.is_nan() { waiting } else { self.rtt.max(waiting) }
    }
}

/// a free-roam snapshot (runtime/roam_core.h RC_MSG_SNAP, version 1): a frame of kind 1 whose datagram (past the
/// switch's 16-byte header) begins "N2FR", 1, 3
pub(crate) fn is_snap(frame: &[u8]) -> bool {
    frame.len() > 21 && &frame[16..20] == b"N2FR" && frame[20] == 1 && frame[21] == 3
}

thread_local! {
    /// xorshift64 per thread: a share's pick is on the switch's hottest path (the OS's numbers are a system call)
    static RNG: Cell<u64> = Cell::new(nfsu2_util::rand::u64() | 1);
}

/// true with the probability share / SHARE_FULL
pub(crate) fn pick(share: u32) -> bool {
    RNG.with(|r| {
        let mut x = r.get();
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        r.set(x);
        ((x >> 32) as u32 & (SHARE_FULL - 1)) < share
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// a ping queued at `sent`, answered at `at`: what pong says, and the share after it
    fn answer(p: &mut Probe, share: &AtomicU32, sent: f64, at: f64) -> (Option<bool>, u32) {
        let d = p.ping(sent);
        let r = p.pong(&d, at, share);
        (r, share.load(Relaxed))
    }

    // the share: halved once a round trip while late, the least past LAG_SEVERE_MS, an eighth back while not
    #[test]
    fn the_share_follows_the_round_trips() {
        let share = AtomicU32::new(SHARE_FULL);
        let mut p = Probe::new(0.0);
        // a path of 80 ms: all of them
        for t in 0..5 {
            let t = t as f64 * 1000.0;
            assert_eq!(answer(&mut p, &share, t, t + 80.0).1, SHARE_FULL);
        }
        // 600 ms waiting: halved, once for pings queued before that cut, again for one queued after it
        let a = p.ping(5000.0);
        let b = p.ping(5100.0);
        p.pong(&a, 5680.0, &share);
        assert_eq!(share.load(Relaxed), SHARE_FULL / 2);
        p.pong(&b, 5780.0, &share);
        assert_eq!(share.load(Relaxed), SHARE_FULL / 2, "a ping queued before the cut does not cut again");
        assert_eq!(answer(&mut p, &share, 6000.0, 6700.0).1, SHARE_FULL / 4);
        // 2 s: the least at once
        assert_eq!(answer(&mut p, &share, 7000.0, 9100.0).1, SHARE_MIN);
        // between low and high: held
        assert_eq!(answer(&mut p, &share, 10000.0, 10300.0).1, SHARE_MIN);
        // back to the base: an eighth a round trip, never past all
        let mut s = SHARE_MIN;
        for k in 0..10 {
            let t = 11000.0 + k as f64 * 1000.0;
            let now = answer(&mut p, &share, t, t + 90.0).1;
            assert!(now > s || now == SHARE_FULL, "{now} after {s}");
            s = now;
        }
        assert_eq!(s, SHARE_FULL);
    }

    // the base: the lowest round trip of the last 30-60 s, at most BASE_MAX_MS
    #[test]
    fn the_base_is_the_path_without_queues() {
        let share = AtomicU32::new(SHARE_FULL);
        // a far path (350 ms) is its own base: never cut
        let mut p = Probe::new(0.0);
        for t in 0..10 {
            let t = t as f64 * 1000.0;
            assert_eq!(answer(&mut p, &share, t, t + 350.0 + (t / 1000.0) * 10.0).1, SHARE_FULL);
        }
        // a path queued from its first ping does not count as its base
        let mut q = Probe::new(0.0);
        assert_eq!(answer(&mut q, &share, 0.0, 3000.0).1, SHARE_MIN);
        // a base two windows old is forgotten: the path got longer for good (a new route, 500 ms), it counts as
        // waiting until then (cut to the least), and as its own base (at most BASE_MAX_MS) after
        let share = AtomicU32::new(SHARE_FULL);
        let mut r = Probe::new(0.0);
        answer(&mut r, &share, 0.0, 50.0);
        for k in 1..=58 {
            let t = k as f64 * 1000.0;
            answer(&mut r, &share, t, t + 500.0);
        }
        assert_eq!(share.load(Relaxed), SHARE_MIN);
        for k in 59..=71 {
            let t = k as f64 * 1000.0;
            answer(&mut r, &share, t, t + 500.0);
        }
        assert_eq!(share.load(Relaxed), SHARE_FULL);
    }

    // a path longer than BASE_MAX_MS without queues (a satellite link): cut while it looks late, its share back once
    // two windows of a cut share show its round trips are that long anyway; a queue on top of it cuts again
    #[test]
    fn a_long_path_gets_its_share_back() {
        // the review of 2026-10-23: 650 ms with one spike to 850 stayed at half for good, 900 ms at the least
        for (rtt, spike) in [(650.0, 850.0), (900.0, 900.0)] {
            let share = AtomicU32::new(SHARE_FULL);
            let mut p = Probe::new(0.0);
            let mut cut = false;
            for k in 0..200 {
                let t = k as f64 * 1000.0;
                let s = answer(&mut p, &share, t, t + if k == 20 { spike } else { rtt }).1;
                cut |= s < SHARE_FULL;
                if k >= 120 {
                    assert_eq!(s, SHARE_FULL, "{rtt} ms: all again at {k} s");
                }
            }
            assert!(cut, "{rtt} ms: cut first, as a path that may be queued");
            // queued on top of it: cut again
            assert_eq!(answer(&mut p, &share, 200_000.0, 200_000.0 + rtt + 600.0).1, SHARE_FULL / 2);
            assert_eq!(answer(&mut p, &share, 201_000.0, 201_000.0 + rtt + 2000.0).1, SHARE_MIN);
        }
        // a path queued from the start that drains under the cut share: its base is the drained one, never raised
        let share = AtomicU32::new(SHARE_FULL);
        let mut q = Probe::new(0.0);
        for k in 0..10 {
            let t = k as f64 * 1000.0;
            answer(&mut q, &share, t, t + 3000.0 - k as f64 * 250.0);
        }
        for k in 10..130 {
            let t = k as f64 * 1000.0;
            answer(&mut q, &share, t, t + 120.0);
        }
        assert_eq!(share.load(Relaxed), SHARE_FULL);
        assert_eq!(q.floor, BASE_MAX_MS);
        assert_eq!(answer(&mut q, &share, 130_000.0, 130_000.0 + 700.0).1, SHARE_FULL / 2);
    }

    // a pong: ours or not, and whether it says the client is there (a ping queued since the keepalive's tick)
    #[test]
    fn a_pong_is_ours_and_a_sign_of_life_only_for_a_ping_since_the_tick() {
        let share = AtomicU32::new(SHARE_FULL);
        let mut p = Probe::new(0.0);
        let old = p.ping(100.0);
        p.tick(500.0);
        let new = p.ping(600.0);
        assert_eq!(p.pong(b"not ours", 700.0, &share), None);
        assert_eq!(p.pong(&old, 700.0, &share), Some(false), "a ping from before the tick times, does not count");
        assert_eq!(p.pong(&new, 750.0, &share), Some(true));
        assert_eq!(p.pong(&new, 760.0, &share), None, "answered once");
        // the newest of several answered: those before it go, unanswered
        let a = p.ping(1000.0);
        let b = p.ping(1100.0);
        let c = p.ping(1200.0);
        assert_eq!(p.pong(&c, 1300.0, &share), Some(true));
        assert_eq!(p.pong(&a, 1310.0, &share), None);
        assert_eq!(p.pong(&b, 1320.0, &share), None);
    }

    // how far behind: the last round trip, or a ping waiting longer
    #[test]
    fn lag_counts_a_ping_still_waiting() {
        let share = AtomicU32::new(SHARE_FULL);
        let mut p = Probe::new(0.0);
        assert_eq!(p.lag(0.0), 0.0);
        answer(&mut p, &share, 0.0, 120.0);
        assert_eq!(p.lag(500.0), 120.0);
        p.ping(1000.0);
        assert_eq!(p.lag(1050.0), 120.0);
        assert_eq!(p.lag(4000.0), 3000.0);
        // never more than MAX_OUT waiting
        for t in 0..200 {
            p.ping(5000.0 + t as f64);
        }
        assert_eq!(p.out.len(), MAX_OUT);
    }

    #[test]
    fn snapshots_and_picks() {
        let mut f = vec![1u8, 0, 0, 0, 10, 77, 0, 2, 10, 77, 1, 2, 0, 1, 0, 2];
        f.extend_from_slice(b"N2FR\x01\x03\x00\x00");
        assert!(is_snap(&f));
        f[21] = 1;
        assert!(!is_snap(&f), "a HELLO");
        f[21] = 3;
        f[20] = 2;
        assert!(!is_snap(&f), "another version");
        assert!(!is_snap(&f[..21]));
        // picks: about the share, and every one at the full share
        let n = 100_000;
        let kept = (0..n).filter(|_| pick(SHARE_FULL / 4)).count();
        assert!((23_000..27_000).contains(&kept), "{kept} of {n} at a quarter");
        assert!((0..1000).all(|_| pick(SHARE_FULL)));
        assert!((0..1000).all(|_| !pick(0)));
    }
}
