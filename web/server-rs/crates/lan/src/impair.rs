//! --lan-impair (a test aid, only with the server's --dev): LAN play's frames (kinds 3-5) between browsers get
//! tools/vlanhub.py's impairments: UDP and ICMP are delayed by delay +- jitter ms, lost with loss %, reordered with
//! reorder % (2 x delay + 5 ms more); TCP (kind 4) is only delayed, in order.  Free roam's frames are never touched
//! (free roam has its own knob in the runtime).
//!
//! `hold=PCT,holdms=MS` is what a player's WebSocket does when a TCP segment is lost on the way: everything after it
//! waits for its resend, then comes at once.  A UDP or ICMP datagram starts a hold of holdms on its link (one sender to
//! one receiver) with hold %, and that link's datagrams wait until it ends (theirs, if later), then go in the order they
//! came.  Not in vlanhub.py.
//!
//! Legs are each player's own way to the server: `leg=MS` (`leg.N=MS` for the browser whose address ends in host
//! number N, e.g. leg.3 for 10.77.0.3, the second one on the switch) delays everything a browser sends on its way up,
//! and everything it receives on its way down, +- `legjitter` ms, in order (one WebSocket: nothing overtakes).  A
//! frame from A to B takes A's leg up, then B's leg down, the way down taken in the order frames reach the server.
//! `stall=PCT,stallms=MS` (`stall.N=PCT` for one browser): with PCT % per frame on a leg, that leg stops for stallms,
//! then hands over all it held at once (a lost TCP segment on that player's line).  So a LAN race's star (the game's
//! creator passing every joiner's car on) costs a joiner's car to another joiner four legs, a direct send two.

use std::cmp::Reverse;
use std::collections::{BinaryHeap, HashMap};
use std::sync::{Arc, Weak};
use std::time::Duration;

use bytes::Bytes;
use parking_lot::Mutex;
use serde_json::{Map, Value};
use tokio::sync::mpsc;
use tokio::time::Instant;

use crate::text::to_number;
use crate::{KIND_STREAM, LanSwitch, Peer, Stats};

pub(crate) struct Impair {
    delay: f64,
    jitter: f64,
    loss: f64,
    reorder: f64,
    hold: f64,
    holdms: f64,
    seed: f64,
    /// the random numbers: a xorshift32 from `seed` (0 is 1)
    x: Mutex<u32>,
    /// when the last TCP segment from one browser to another goes out (they stay in order)
    tcp_last: Mutex<HashMap<(u32, u32), f64>>,
    /// until when a link of one browser to another holds its UDP and ICMP (`hold`), and when the last one held goes
    held: Mutex<HashMap<(u32, u32), (Instant, Instant)>>,
    /// the legs (`leg`, `leg.N`, `legjitter`, `stall`, `stall.N`, `stallms`); host numbers of the per-browser ones
    leg: f64,
    legjitter: f64,
    stall: f64,
    stallms: f64,
    leg_of: HashMap<u32, f64>,
    stall_of: HashMap<u32, f64>,
    /// each leg (a browser's address, up): when its last frame goes, and until when it stalls
    legs: Mutex<HashMap<(u32, bool), (Instant, Instant)>>,
    tx: mpsc::UnboundedSender<Delayed>,
    rx: Mutex<Option<mpsc::UnboundedReceiver<Delayed>>>,
}

struct Delayed {
    at: Instant,
    seq: u64,
    from: Arc<Peer>,
    to: Arc<Peer>,
    data: Bytes,
    dst: u32,
    /// on its way up still (legs): its way down is taken when it reaches the server
    up: bool,
}

/// a browser's host number on the switch (10.77.0.3 -> 3; lib.rs MASK)
fn host_of(vip: u32) -> u32 {
    vip & 0x1fff
}

/// a number as an unsigned 32-bit integer: its whole part modulo 2^32 (0 for one that is not finite)
fn to_uint32(x: f64) -> u32 {
    if !x.is_finite() {
        return 0;
    }
    (x.trunc().rem_euclid(4_294_967_296.0)) as u32
}

impl Impair {
    /// the --lan-impair spec: "delay=80,jitter=0,loss=100,seed=3"; unknown keys and non-numbers are ignored
    pub(crate) fn parse(spec: &str) -> Option<Impair> {
        if spec.is_empty() {
            return None;
        }
        let (mut delay, mut jitter, mut loss, mut reorder, mut seed) = (0.0, 0.0, 0.0, 0.0, 1.0);
        let (mut hold, mut holdms) = (0.0, 300.0);
        let (mut leg, mut legjitter, mut stall, mut stallms) = (0.0, 0.0, 0.0, 1000.0);
        let (mut leg_of, mut stall_of) = (HashMap::new(), HashMap::new());
        for kv in spec.split(',') {
            let mut it = kv.split('=');
            let k = it.next().unwrap_or("");
            let Some(v) = it.next().map(to_number).filter(|v| v.is_finite()) else { continue };
            match k {
                "delay" => delay = v,
                "jitter" => jitter = v,
                "loss" => loss = v,
                "reorder" => reorder = v,
                "hold" => hold = v,
                "holdms" => holdms = v,
                "seed" => seed = v,
                "leg" => leg = v,
                "legjitter" => legjitter = v,
                "stall" => stall = v,
                "stallms" => stallms = v,
                _ => {
                    let per = |p: &str| k.strip_prefix(p).and_then(|n| n.parse::<u32>().ok());
                    if let Some(n) = per("leg.") {
                        leg_of.insert(n, v);
                    } else if let Some(n) = per("stall.") {
                        stall_of.insert(n, v);
                    }
                }
            }
        }
        let x = match to_uint32(seed) {
            0 => 1,
            x => x,
        };
        let (tx, rx) = mpsc::unbounded_channel();
        Some(Impair {
            delay,
            jitter,
            loss,
            reorder,
            hold,
            holdms,
            seed,
            x: Mutex::new(x),
            tcp_last: Mutex::new(HashMap::new()),
            held: Mutex::new(HashMap::new()),
            leg,
            legjitter,
            stall,
            stallms,
            leg_of,
            stall_of,
            legs: Mutex::new(HashMap::new()),
            tx,
            rx: Mutex::new(Some(rx)),
        })
    }

    /// whether frames take the browsers' legs
    fn legs_on(&self) -> bool {
        self.leg != 0.0 || self.legjitter != 0.0 || self.stall != 0.0 || !self.leg_of.is_empty() || !self.stall_of.is_empty()
    }

    /// when a frame that reaches one end of a browser's leg at `from` leaves the other end (up: the browser's way
    /// to the server, else back): its delay +- legjitter, after a stall of the leg, never before the leg's last
    fn leg_time(&self, vip: u32, up: bool, from: Instant) -> Instant {
        let h = host_of(vip);
        let mut d = *self.leg_of.get(&h).unwrap_or(&self.leg);
        if self.legjitter != 0.0 {
            d += (self.rand() * 2.0 - 1.0) * self.legjitter;
        }
        let stall = *self.stall_of.get(&h).unwrap_or(&self.stall);
        let start = stall != 0.0 && self.rand() * 100.0 < stall;
        let mut legs = self.legs.lock();
        let (last, until) = legs.entry((vip, up)).or_insert((from, from));
        if start && *until <= from {
            *until = from + Duration::from_secs_f64(self.stallms.max(0.0) / 1000.0);
        }
        let t = (from + Duration::from_secs_f64(d.max(0.0) / 1000.0)).max(*until).max(*last);
        *last = t;
        t
    }

    /// what info() shows of it
    pub(crate) fn spec_json(&self) -> Value {
        let mut m = Map::new();
        for (k, v) in [("delay", self.delay), ("jitter", self.jitter), ("loss", self.loss), ("reorder", self.reorder), ("seed", self.seed)]
        {
            m.insert(k.into(), nfsu2_util::json::num(v));
        }
        if self.hold != 0.0 {
            m.insert("hold".into(), nfsu2_util::json::num(self.hold));
            m.insert("holdms".into(), nfsu2_util::json::num(self.holdms));
        }
        if self.legs_on() {
            for (k, v) in [("leg", self.leg), ("legjitter", self.legjitter), ("stall", self.stall), ("stallms", self.stallms)] {
                m.insert(k.into(), nfsu2_util::json::num(v));
            }
            let mut per: Vec<(String, f64)> = self.leg_of.iter().map(|(n, v)| (format!("leg.{n}"), *v)).collect();
            per.extend(self.stall_of.iter().map(|(n, v)| (format!("stall.{n}"), *v)));
            per.sort_by(|a, b| a.0.cmp(&b.0));
            for (k, v) in per {
                m.insert(k, nfsu2_util::json::num(v));
            }
        }
        Value::Object(m)
    }

    fn rand(&self) -> f64 {
        let mut x = self.x.lock();
        *x ^= *x << 13;
        *x ^= *x >> 17;
        *x ^= *x << 5;
        *x as f64 / 4_294_967_296.0
    }

    /// a frame of LAN play's between browsers: lost, or delivered later (through the queue rules then)
    pub(crate) fn deliver(&self, stats: &Stats, from: &Arc<Peer>, p: &Arc<Peer>, data: &Bytes, dst: u32) {
        let mut d = self.delay;
        if data[0] == KIND_STREAM {
            let now = nfsu2_util::now_ms() as f64;
            let mut last = self.tcp_last.lock();
            let e = last.entry((from.vip, p.vip)).or_insert(0.0);
            let t = (now + d).max(*e);
            *e = t;
            d = t - now;
        } else {
            if self.loss != 0.0 && self.rand() * 100.0 < self.loss {
                stats.lost.inc();
                return;
            }
            if self.jitter != 0.0 {
                d += (self.rand() * 2.0 - 1.0) * self.jitter;
            }
            if self.reorder != 0.0 && self.rand() * 100.0 < self.reorder {
                d += 2.0 * self.delay + 5.0;
            }
        }
        // under 1 ms is 1 ms (as the Node server's timers had it)
        let now = Instant::now();
        let mut at = now + Duration::from_secs_f64(d.max(1.0) / 1000.0);
        if self.hold != 0.0 && data[0] != KIND_STREAM {
            // a held link's datagrams go when the hold ends, in order (never before the one before them)
            let start = self.rand() * 100.0 < self.hold;
            let mut held = self.held.lock();
            let (until, last) = held.entry((from.vip, p.vip)).or_insert((now, now));
            if start && *until <= now {
                *until = now + Duration::from_secs_f64(self.holdms.max(0.0) / 1000.0);
            }
            if *until > now {
                at = at.max(*until).max(*last);
                *last = at;
            }
        }
        let up = self.legs_on();
        if up {
            at = self.leg_time(from.vip, true, at);
        }
        let _ = self.tx.send(Delayed { at, seq: 0, from: from.clone(), to: p.clone(), data: data.clone(), dst, up });
    }

    /// a browser left: its TCP order and its legs are forgotten
    pub(crate) fn forget(&self, vip: u32) {
        self.tcp_last.lock().retain(|k, _| k.0 != vip && k.1 != vip);
        self.held.lock().retain(|k, _| k.0 != vip && k.1 != vip);
        self.legs.lock().retain(|k, _| k.0 != vip);
    }

    /// the delayed frames go out in the order of their times (equal times in the order they came)
    pub(crate) fn start(&self, sw: Weak<LanSwitch>) {
        let Some(mut rx) = self.rx.lock().take() else { return };
        tokio::spawn(async move {
            let mut heap: BinaryHeap<Reverse<(Instant, u64)>> = BinaryHeap::new();
            let mut held: HashMap<u64, Delayed> = HashMap::new();
            let mut seq = 0u64;
            loop {
                let next = heap.peek().map(|Reverse((at, _))| *at);
                let got = match next {
                    Some(at) => tokio::select! {
                        m = rx.recv() => m.map(Some),
                        _ = tokio::time::sleep_until(at) => Some(None),
                    },
                    None => rx.recv().await.map(Some),
                };
                match got {
                    None => return,
                    Some(Some(mut d)) => {
                        seq += 1;
                        d.seq = seq;
                        heap.push(Reverse((d.at, seq)));
                        held.insert(seq, d);
                    }
                    Some(None) => {
                        let Some(sw) = sw.upgrade() else { return };
                        let now = Instant::now();
                        while let Some(Reverse((at, s))) = heap.peek().copied() {
                            if at > now {
                                break;
                            }
                            heap.pop();
                            let Some(mut d) = held.remove(&s) else { continue };
                            // at the server: the receiver's leg down next (in the order frames got here)
                            if d.up {
                                if let Some(im) = &sw.impair {
                                    d.up = false;
                                    d.at = im.leg_time(d.to.vip, false, at);
                                    seq += 1;
                                    d.seq = seq;
                                    heap.push(Reverse((d.at, seq)));
                                    held.insert(seq, d);
                                    continue;
                                }
                            }
                            sw.deliver_now(&d.from, &d.to, &d.data, d.dst);
                        }
                    }
                }
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn spec_and_random_numbers() {
        let im = Impair::parse("delay=80,jitter=0,loss=100,seed=3,bogus=1,reorder=x").unwrap();
        assert_eq!(nfsu2_util::json::stringify(&im.spec_json()), r#"{"delay":80,"jitter":0,"loss":100,"reorder":0,"seed":3}"#);
        // x = 3: the xorshift32's first step, by hand
        let mut x: u32 = 3;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        assert_eq!(im.rand(), x as f64 / 4_294_967_296.0);
        assert!(Impair::parse("").is_none());
        assert_eq!(to_uint32(-1.0), u32::MAX);
        assert_eq!(to_uint32(4_294_967_297.5), 1);
    }

    #[tokio::test]
    async fn legs_in_order_per_browser_and_stalls() {
        let im = Impair::parse("leg=100,leg.3=40,legjitter=30,stall.4=100,stallms=500,seed=7").unwrap();
        assert!(im.legs_on());
        assert_eq!(
            nfsu2_util::json::stringify(&im.spec_json()),
            r#"{"delay":0,"jitter":0,"loss":0,"reorder":0,"seed":7,"leg":100,"legjitter":30,"stall":0,"stallms":500,"leg.3":40,"stall.4":100}"#
        );
        let (a, b, c) = (0x0a4d_0002u32, 0x0a4d_0003u32, 0x0a4d_0004u32);
        let t0 = Instant::now();
        let ms = |t: Instant| (t - t0).as_secs_f64() * 1000.0;
        // 100 +- 30 ms, never before the frame before it on the same leg
        let mut last = 0.0;
        for i in 0..50 {
            let t = ms(im.leg_time(a, true, t0 + Duration::from_millis(i)));
            assert!(t >= last && t >= i as f64 + 69.9 && t <= i as f64 + 130.1, "{t}");
            last = t;
        }
        // leg.3: 40 +- 30; its way down is its own leg (in order with nothing on its way up)
        let t = ms(im.leg_time(b, false, t0));
        assert!((9.9..=70.1).contains(&t), "{t}");
        // stall.4=100: the first frame stalls the leg for 500 ms, and the next ones wait for it too
        let t1 = ms(im.leg_time(c, true, t0));
        let t2 = ms(im.leg_time(c, true, t0 + Duration::from_millis(100)));
        assert!(t1 >= 499.9 && t2 >= t1, "{t1} {t2}");
        // none of it without the keys
        assert!(!Impair::parse("delay=80").unwrap().legs_on());
    }
}
