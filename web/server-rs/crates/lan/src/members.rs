//! Who is on the switch (admission, a connection's close, the free addresses, "LAN full", the reservations, the peer
//! list's changes, the store): all of it under the one `members` lock, which routing never takes.

use std::sync::Arc;
use std::sync::atomic::Ordering::Relaxed;
use std::time::Duration;

use bytes::Bytes;
use indexmap::{IndexMap, IndexSet};
use nfsu2_util::json::write_str;
use nfsu2_util::{Bucket, now_ms, vip_str};
use serde_json::{Map, Value};

use crate::conn::{Conn, Out};
use crate::queue::Line;
use crate::text::{js_string, new_token, token_fits, token_hash};
use crate::{ADDRS, AttachCtx, CtlBuckets, LAN_OFF, LAN_ON, LanSwitch, MASK, Pair, Peer, slot, vip_at};

/// A departed holder's address, kept for its token only.
pub(crate) struct Reservation {
    pub(crate) hash: [u8; 32],
    pub(crate) until: i64,
    /// the client (group) and site it was reserved for (none for one from the store)
    pub(crate) group: Option<Arc<str>>,
    pub(crate) site: Option<Arc<str>>,
}

pub(crate) struct Members {
    /// in the order they came (a replaced holder keeps its place): the peer lists, info and the store show it
    pub(crate) peers: IndexMap<u32, Arc<Peer>>,
    /// the ones of `peers` whose LAN play is not off (in no order: LAN play's broadcasts, `LanSwitch::lan_all`)
    lan: IndexMap<u32, Arc<Peer>>,
    pub(crate) reserved: IndexMap<u32, Reservation>,
    next: usize,
    /// the peer list: addresses that joined, got their name or left since the last change went out, and the ones
    /// that left; when the whole list last went to the clients that take no changes
    changed: IndexSet<u32>,
    left: IndexSet<u32>,
    delta_armed: bool,
    full_armed: bool,
    full_at: f64,
    /// the store: a save waits (`era` tells a stale timer), the order of what was saved
    save_armed: bool,
    save_gen: u64,
    save_seq: u64,
    /// the pages waiting for a place (queue.rs)
    pub(crate) line: Line,
}

impl Members {
    pub(crate) fn new() -> Self {
        Members {
            peers: IndexMap::new(),
            lan: IndexMap::new(),
            reserved: IndexMap::new(),
            next: 0,
            changed: IndexSet::new(),
            left: IndexSet::new(),
            delta_armed: false,
            full_armed: false,
            full_at: f64::NEG_INFINITY,
            save_armed: false,
            save_gen: 0,
            save_seq: 0,
            line: Line::default(),
        }
    }

    /// a free address that is nobody's reservation, or 0: a reservation is never given to anyone but its token (the
    /// review of the audit's F1, in the crate doc).  (The expired ones went just before: `LanSwitch::expire`.)
    pub(crate) fn allocate(&mut self) -> u32 {
        for i in 0..ADDRS {
            let k = (self.next + i) % ADDRS;
            let c = vip_at(k);
            if !self.peers.contains_key(&c) && !self.reserved.contains_key(&c) {
                self.next = (k + 1) % ADDRS;
                return c;
            }
        }
        0
    }

    /// a newcomer has no place now: every place is taken, by a player or kept for one who dropped (a reservation holds
    /// its place as it holds its address, also where there are more addresses than places), so also every address
    /// (no address is both held and kept)
    pub(crate) fn full(&self, max_peers: usize) -> bool {
        let now = now_ms();
        self.peers.len() + self.reserved.values().filter(|r| r.until > now).count() >= max_peers.min(ADDRS)
    }
}

impl LanSwitch {
    /// A connection admitted: the address (given back only to the token that held it), the welcome, the peer list.
    /// None: not admitted now (closed already; "LAN full"; or, for a page that can wait, in line: queue.rs, the reader
    /// finds its peer once the line admits it).
    pub(crate) fn admit(&self, conn: &Arc<Conn>, ctx: &Arc<AttachCtx>, hello: Option<Map<String, Value>>) -> Option<Arc<Peer>> {
        if !conn.open() {
            return None;
        }
        let mut m = self.members.lock();
        let mut held = held_address(&m, hello.as_ref());
        // a page back with its ticket: its place in line again (it never had an address), or the place the line gave
        // it as its link went (its welcome lost with it: queue.rs `seated`)
        if held.0 == 0 && ctx.queue {
            if let Some(Value::String(t)) = hello.as_ref().and_then(|h| h.get("ticket")) {
                let t = t.clone();
                if self.rejoin(&mut m, conn, ctx, hello.clone(), &t) {
                    drop(m);
                    self.let_in();
                    return conn.peer.lock().clone();
                }
                held = m.line.seated_for(&m.peers, &m.reserved, &t);
            }
        }
        // a newcomer waits while anyone does
        let place = self.place(&mut m, held, false);
        match place {
            Some((vip, old, resumed)) => {
                let (peer, count) = self.seat(&mut m, conn, ctx, hello, vip, old);
                drop(m);
                if let Some(f) = &ctx.on_join {
                    f(peer.vip, resumed, count);
                }
                Some(peer)
            }
            None => {
                if !(ctx.queue && self.enqueue(&mut m, conn, ctx, hello)) {
                    self.stats.refused.inc();
                    drop(m);
                    conn.close(lan_full());
                }
                None
            }
        }
    }

    /// A place for one who asks: the address its token held (`held`: it, and the connection of the same client it
    /// replaces, which takes no new place; a reservation's place was never free), or a free one; None when there is
    /// none for it now: the switch full, or (unless `first`, the line's first) others waiting before it.  A resumed
    /// address leaves its reservation here.
    pub(crate) fn place(&self, m: &mut Members, held: (u32, Option<Arc<Peer>>), first: bool) -> Option<(u32, Option<Arc<Peer>>, bool)> {
        self.expire(m);
        let (vip, old) = held;
        if vip != 0 {
            // (more players than places only after a restart with fewer: the store's reservations)
            if old.is_none() && m.peers.len() >= self.o.max_peers {
                return None;
            }
            m.reserved.shift_remove(&vip);
            self.stats.resumed.inc();
            return Some((vip, old, true));
        }
        if (!first && m.line.before_newcomers()) || m.full(self.o.max_peers) {
            return None;
        }
        match m.allocate() {
            0 => None,
            v => Some((v, None, false)),
        }
    }

    /// The connection takes its place: its peer, the welcome, the peer list, the table; the holder it replaces goes.
    /// The players on the switch now come back with it (for `on_join`, which the caller runs once the lock is gone).
    pub(crate) fn seat(
        &self,
        m: &mut Members,
        conn: &Arc<Conn>,
        ctx: &AttachCtx,
        hello: Option<Map<String, Value>>,
        vip: u32,
        old: Option<Arc<Peer>>,
    ) -> (Arc<Peer>, usize) {
        let o = &self.o;
        let token = new_token();
        let now = nfsu2_util::mono_ms();
        // (for its writer: a compact bundle leaves out the address a datagram was sent to, its own)
        conn.vip.store(vip, std::sync::atomic::Ordering::Relaxed);
        let peer = Arc::new(Peer {
            vip,
            conn: conn.clone(),
            group: ctx.group.clone(),
            site: ctx.site.clone(),
            token_hash: token_hash(&token),
            name: Default::default(),
            hello: false.into(),
            deltas: false.into(),
            bundle: false.into(),
            lan: crate::LAN_NULL.into(),
            frames_in: 0.into(),
            frames_out: 0.into(),
            fed_online: false.into(),
            buckets: parking_lot::Mutex::new(Pair::new(o.frame_rate, o.byte_rate)),
            ctl: parking_lot::Mutex::new(CtlBuckets { texts: Bucket::new_at(o.text_rate, now), ctl: Bucket::new_at(o.ctl_rate, now) }),
            alive: true.into(),
            last_in: now_ms().into(),
            probe: parking_lot::Mutex::new(crate::lag::Probe::new(now)),
            share: crate::lag::SHARE_FULL.into(),
            listed: false.into(),
            ident: ctx.ident.clone(),
            since: now_ms(),
        });
        if let Some(h) = &hello {
            peer.ctl.lock().texts.take_at(now, 1.0);
            greet(&peer, h, self.o.fpred);
            self.hand_blocks(vip, h);
        }
        m.peers.insert(vip, peer.clone());
        // (a replaced holder's place on it goes with it)
        m.lan.swap_remove(&vip);
        if peer.lan() != Some(false) {
            m.lan.insert(vip, peer.clone());
        }
        *conn.peer.lock() = Some(peer.clone());
        // the welcome (and a resume's whole list, which has the newcomer on it) are queued before the other players'
        // readers can find the newcomer: nothing they route to it comes first (Node sent both before it went back to
        // its event loop)
        let welcome = format!(r#"{{"t":"welcome","vip":{vip},"vipStr":"{}","mask":"{MASK}","token":"{token}","bundle":1}}"#, vip_str(vip));
        conn.push(Out::Text(Bytes::from(welcome)));
        if peer.hello.load(Relaxed) {
            let list = self.list_for(m, &peer);
            self.send(&peer, Out::Text(list));
        }
        if let Some(i) = slot(vip) {
            self.table[i].store(Some(peer.clone()));
        }
        self.publish(m);
        // the same client's older connection (it died without a close): it goes, and its late close changes nothing
        if let Some(old) = old {
            old.conn.terminate();
        }
        self.peers_changed(m, vip, false);
        self.persist(m);
        (peer, m.peers.len())
    }

    /// the 'close' of a connection: its client's limits let go, and for the holder of an address, the address
    /// reserved for its token (not when the page closed the connection itself, 1000 "idle" or "closed": no network
    /// feature in use, or the game ended; such a page comes back without a token, so its address is free at once; nor
    /// when the operator removed it).  It is kept a while for the operator's `who`.
    /// An old connection's late close must not take the address from a new holder.
    pub(crate) fn closed(&self, conn: &Arc<Conn>, ctx: &AttachCtx) {
        self.leave_group(false, &ctx.group);
        if let Some(s) = &ctx.site {
            self.leave_group(true, s);
        }
        let Some(peer) = conn.peer.lock().take() else {
            self.left_line(conn);
            return;
        };
        self.streams.lock().remove(conn.id);
        conn.streaming.store(false, Relaxed);
        let vip = peer.vip;
        let mut m = self.members.lock();
        if !m.peers.get(&vip).is_some_and(|p| Arc::ptr_eq(p, &peer)) {
            return;
        }
        m.peers.shift_remove(&vip);
        m.lan.swap_remove(&vip);
        if let Some(i) = slot(vip) {
            self.table[i].store(None);
        }
        self.publish(&m);
        if conn.let_go() {
            self.stats.released.inc();
        } else {
            self.reserve(&mut m, vip, &peer);
        }
        self.persist(&mut m);
        self.peers_changed(&mut m, vip, true);
        let count = m.peers.len();
        // the hosts hear of it before the lock goes: a page that resumes this address can only be admitted after, so
        // its first frames never reach a host before the old connection's departure (which would take the new
        // connection's streams with it); posting only queues
        if let Some(h) = &self.online {
            h.peer_gone(vip);
        }
        if let Some(h) = &self.roam {
            h.peer_gone(vip);
        }
        drop(m);
        {
            let mut g = self.gone.lock();
            if g.len() >= crate::GONE_MAX {
                g.pop_front();
            }
            g.push_back(peer.who("", Some(now_ms())));
        }
        if let Some(im) = &self.impair {
            im.forget(vip);
        }
        if let Some(f) = &ctx.on_leave {
            f(vip, peer.name(), count);
        }
        // (its place to the line's first, when it went back at once)
        self.let_in();
    }

    /// the copy-on-write lists of every peer (in the order of `peers`) and of those with LAN play open, after a change
    fn publish(&self, m: &Members) {
        self.all.store(Arc::new(m.peers.values().cloned().collect()));
        self.publish_lan(m);
    }

    fn publish_lan(&self, m: &Members) {
        self.lan_all.store(Arc::new(m.lan.values().cloned().collect()));
    }

    /// peer's LAN play, from its hello or its {"t":"lan"} (only its own reader sets it, as its connection's close
    /// logic runs after that reader ended): on the list of those with LAN play open or not, while it holds its address
    pub(crate) fn set_lan(&self, peer: &Arc<Peer>, on: bool) {
        let mut m = self.members.lock();
        peer.lan.store(if on { LAN_ON } else { LAN_OFF }, Relaxed);
        self.lan_changed(&mut m, peer);
        // the peer list follows LAN play (`list_for`): the whole list as it opens, no changes while it is shut
        if !m.peers.get(&peer.vip).is_some_and(|p| Arc::ptr_eq(p, peer)) || !peer.hello.load(Relaxed) {
            return;
        }
        if !on {
            peer.listed.store(false, Relaxed);
        } else if !peer.listed.swap(true, Relaxed) {
            let list = self.peer_list(&m);
            self.send(peer, Out::Text(list));
        }
    }

    /// the list a peer gets with its hello: the whole list, and the changes after it (`listed`), unless its LAN play is
    /// shut: then only the server's own hosts (the Online host, which the game connects to as it opens the Online
    /// screens, before the whole list could come).  Only LAN play needs the other players' addresses (the module's
    /// peer bits answer a connect to an address nobody holds at once; runtime/web/net_lan.c); free roam's come from
    /// its hub.  A page of before says nothing about LAN play and gets it all, as it did.
    fn list_for(&self, m: &Members, peer: &Peer) -> Bytes {
        let all = peer.lan() != Some(false);
        peer.listed.store(all, Relaxed);
        if all { self.peer_list(m) } else { self.hosts_list() }
    }

    fn lan_changed(&self, m: &mut Members, peer: &Arc<Peer>) {
        let vip = peer.vip;
        if !m.peers.get(&vip).is_some_and(|p| Arc::ptr_eq(p, peer)) {
            return;
        }
        let want = peer.lan() != Some(false);
        if want == m.lan.contains_key(&vip) {
            return;
        }
        if want {
            m.lan.insert(vip, peer.clone());
        } else {
            m.lan.swap_remove(&vip);
        }
        self.publish_lan(m);
    }

    /// The reservations past their time go.  The hosts hear of each one from the store that goes so (`Host::peer_gone`):
    /// a player of before a restart who did not come back for its address, whose leaving they never saw (the Online
    /// host keeps the races going on over a restart, and their players' names, until it hears they left).  The ones
    /// a departure made, they heard of at the departure.  Whether any went.
    pub(crate) fn expire(&self, m: &mut Members) -> bool {
        let now = now_ms();
        let before = m.reserved.len();
        let mut lapsed = Vec::new();
        m.reserved.retain(|v, r| {
            if r.until > now {
                return true;
            }
            if r.group.is_none() {
                lapsed.push(*v);
            }
            false
        });
        // (under the lock: a page that takes the address later can only be admitted after, so its frames never reach
        // a host before this; posting only queues)
        for vip in lapsed {
            if let Some(h) = &self.online {
                h.peer_gone(vip);
            }
            if let Some(h) = &self.roam {
                h.peer_gone(vip);
            }
        }
        m.reserved.len() != before
    }

    /// vip is kept for its token for `reserve_ms`; past its client's (and site's) share of reservations
    /// (`group_reserve`, `site_reserve`: so that one client cannot reserve them all), that client's own oldest go
    fn reserve(&self, m: &mut Members, vip: u32, peer: &Peer) {
        let now = now_ms();
        let group = peer.group.id.clone();
        let site = peer.site.as_ref().map(|s| s.id.clone());
        m.reserved.insert(
            vip,
            Reservation { hash: peer.token_hash, until: now + self.o.reserve_ms, group: Some(group.clone()), site: site.clone() },
        );
        let trim = |m: &mut Members, by_site: bool, id: &Arc<str>, max: usize| {
            let mut mine: Vec<(u32, i64)> = m
                .reserved
                .iter()
                .filter(|(_, r)| r.until > now && if by_site { r.site.as_ref() == Some(id) } else { r.group.as_ref() == Some(id) })
                .map(|(v, r)| (*v, r.until))
                .collect();
            // the oldest first (a stable sort: equal times stay in the order of `reserved`)
            mine.sort_by_key(|e| e.1);
            for (v, _) in &mine[..mine.len().saturating_sub(max)] {
                m.reserved.shift_remove(v);
                self.stats.evicted.inc();
            }
        };
        trim(m, false, &group, self.o.group_reserve);
        if let Some(site) = &site {
            trim(m, true, site, self.o.site_reserve);
        }
    }

    /// the whole peer list, as a message (the Online host on it as `server: true`)
    pub(crate) fn peer_list(&self, m: &Members) -> Bytes {
        self.list_of(m.peers.values())
    }

    /// the list of the server's own hosts alone
    fn hosts_list(&self) -> Bytes {
        self.list_of(std::iter::empty())
    }

    fn list_of<'a>(&self, peers: impl ExactSizeIterator<Item = &'a Arc<Peer>>) -> Bytes {
        let mut s = String::with_capacity(32 + 48 * peers.len());
        s.push_str(r#"{"t":"peers","peers":["#);
        let mut first = true;
        for p in peers {
            if !first {
                s.push(',');
            }
            first = false;
            entry(&mut s, p.vip, p.name());
        }
        for h in [&self.online, &self.roam].into_iter().flatten() {
            if h.listed() {
                if !first {
                    s.push(',');
                }
                first = false;
                entry(&mut s, h.vip(), h.name());
                s.pop();
                s.push_str(r#","server":true}"#);
            }
        }
        s.push_str("]}");
        Bytes::from(s)
    }

    /// vip joined, got its name or left; everyone hears of it `peers_ms` later, with whatever else changed by then
    pub(crate) fn peers_changed(&self, m: &mut Members, vip: u32, left: bool) {
        m.changed.insert(vip);
        if left {
            m.left.insert(vip);
        }
        if !m.delta_armed {
            m.delta_armed = true;
            self.after(Duration::from_millis(self.o.peers_ms), |sw| sw.send_delta());
        }
        if !m.full_armed {
            m.full_armed = true;
            let ms = (self.o.peers_ms as f64).max(m.full_at + self.o.full_peers_ms - nfsu2_util::mono_ms());
            self.after(Duration::from_secs_f64(ms / 1000.0), |sw| sw.send_full());
        }
    }

    /// a timer of the switch's (ended by `stop()`)
    fn after(&self, d: Duration, f: impl FnOnce(&LanSwitch) + Send + 'static) {
        let me = self.me.clone();
        let stopped = self.stopped.clone();
        tokio::spawn(async move {
            tokio::select! {
                _ = stopped.cancelled() => {}
                _ = tokio::time::sleep(d) => {
                    if let Some(sw) = me.upgrade() {
                        f(&sw);
                    }
                }
            }
        });
    }

    /// the changes, to the clients that take them (sent under the lock, so that a client's
    /// whole list and the changes after it reach it in the order they were made)
    fn send_delta(&self) {
        let mut m = self.members.lock();
        m.delta_armed = false;
        let mut s = String::from(r#"{"t":"delta","peers":["#);
        let mut first = true;
        for v in &m.changed {
            if let Some(p) = m.peers.get(v) {
                if !first {
                    s.push(',');
                }
                first = false;
                entry(&mut s, p.vip, p.name());
            }
        }
        s.push_str(r#"],"gone":["#);
        for (i, v) in m.left.iter().enumerate() {
            if i > 0 {
                s.push(',');
            }
            s.push_str(&v.to_string());
        }
        s.push_str("]}");
        m.changed.clear();
        m.left.clear();
        let msg = Bytes::from(s);
        for p in m.peers.values() {
            if p.deltas.load(Relaxed) && p.listed.load(Relaxed) && p.open() {
                self.send(p, Out::Text(msg.clone()));
            }
        }
    }

    /// the whole list to the clients that take no changes, at most every `full_peers_ms`
    fn send_full(&self) {
        let mut m = self.members.lock();
        m.full_armed = false;
        m.full_at = nfsu2_util::mono_ms();
        if !m.peers.values().any(|p| !p.deltas.load(Relaxed) && p.open()) {
            return;
        }
        let msg = self.peer_list(&m);
        for p in m.peers.values() {
            if !p.deltas.load(Relaxed) && p.open() {
                self.send(p, Out::Text(msg.clone()));
            }
        }
    }

    /// a hello, the connection's first (later ones change nothing): its name and what it takes, the whole list
    pub(crate) fn hello(&self, peer: &Arc<Peer>, msg: &Map<String, Value>) {
        let mut m = self.members.lock();
        if peer.hello.load(Relaxed) {
            return;
        }
        let was = peer.name().to_string();
        greet(peer, msg, self.o.fpred);
        self.hand_blocks(peer.vip, msg);
        self.lan_changed(&mut m, peer);
        let list = self.list_for(&m, peer);
        self.send(peer, Out::Text(list));
        if peer.name() != was {
            self.peers_changed(&mut m, peer.vip, false);
        }
    }

    /// A hello's "block" to the free-roam hub, with the page's address: every connection's hello, a new one's
    /// (`hello`), a resumed one's or one let in from the line (`seat`), as the hub forgets a page's blocks when its
    /// connection goes (`Host::peer_gone`, posted before a resume can take the address).
    fn hand_blocks(&self, vip: u32, hello: &Map<String, Value>) {
        if let Some(h) = &self.roam {
            let names = block_names(hello);
            if !names.is_empty() {
                h.blocks(vip, names);
            }
        }
    }

    /// the store gets the addresses held and reserved, shortly after a change
    pub(crate) fn persist(&self, m: &mut Members) {
        if self.store.is_none() || m.save_armed {
            return;
        }
        m.save_armed = true;
        let era = m.save_gen;
        self.after_always(Duration::from_millis(500), move |sw| {
            if sw.members.lock().save_gen == era {
                sw.flush(false);
            }
        });
    }

    /// a timer that `stop()` does not end (a change after the stop is still saved)
    fn after_always(&self, d: Duration, f: impl FnOnce(&LanSwitch) + Send + 'static) {
        let me = self.me.clone();
        tokio::spawn(async move {
            tokio::time::sleep(d).await;
            if let Some(sw) = me.upgrade() {
                f(&sw);
            }
        });
    }

    /// the store's list, taken under the lock, written on a blocking thread (or here, as the switch stops)
    fn flush(&self, now: bool) {
        let Some(store) = self.store.clone() else { return };
        let (seq, json) = {
            let mut m = self.members.lock();
            m.save_armed = false;
            m.save_gen += 1;
            m.save_seq += 1;
            (m.save_seq, store_list(&m))
        };
        if now {
            store.save(seq, &json);
        } else {
            tokio::task::spawn_blocking(move || store.save(seq, &json));
        }
    }

    pub(crate) fn flush_now(&self) {
        self.flush(true);
    }
}

/// what the store keeps: the addresses held, then the ones reserved, each with its token's hash
fn store_list(m: &Members) -> String {
    let now = now_ms();
    let mut s = String::from("[");
    let mut first = true;
    let mut one = |s: &mut String, v: u32, h: &[u8; 32]| {
        if !first {
            s.push(',');
        }
        first = false;
        s.push_str(r#"{"v":""#);
        s.push_str(&vip_str(v));
        s.push_str(r#"","h":""#);
        s.push_str(&nfsu2_util::js::base64url(h));
        s.push_str(r#""}"#);
    };
    for p in m.peers.values() {
        one(&mut s, p.vip, &p.token_hash);
    }
    for (v, r) in &m.reserved {
        if r.until > now && !m.peers.contains_key(v) {
            one(&mut s, *v, &r.hash);
        }
    }
    s.push(']');
    s
}

/// the address a hello's token held: one reserved for it, or one a connection of the same client holds (which this one
/// replaces); 0 for none
pub(crate) fn held_address(m: &Members, hello: Option<&Map<String, Value>>) -> (u32, Option<Arc<Peer>>) {
    let Some(Value::String(tok)) = hello.and_then(|h| h.get("token")) else { return (0, None) };
    let w = crate::vip_num(&js_string(hello.and_then(|h| h.get("vip"))));
    match m.peers.get(&w) {
        None if m.reserved.get(&w).is_some_and(|r| r.until > now_ms() && token_fits(tok, &r.hash)) => (w, None),
        Some(h) if token_fits(tok, &h.token_hash) => (w, Some(h.clone())),
        _ => (0, None),
    }
}

/// `{"vip":N,"name":"..."}`
fn entry(s: &mut String, vip: u32, name: &str) {
    s.push_str(r#"{"vip":"#);
    s.push_str(&vip.to_string());
    s.push_str(r#","name":"#);
    write_str(s, name);
    s.push('}');
}

/// a full switch's answer (a fixed reason)
pub(crate) fn lan_full() -> Bytes {
    let mut p = 1013u16.to_be_bytes().to_vec();
    p.extend_from_slice(b"LAN full");
    Bytes::from(p)
}

/// a hello's "block": the names (as the game shows them over cars: printable ASCII, at most 24) of the players the page
/// blocks, at most BLOCK_MAX of them, each trimmed; what is not a string, an empty one and those past the cap are left
/// out, a longer one is cut
const BLOCK_MAX: usize = 32;
fn block_names(m: &Map<String, Value>) -> Vec<String> {
    let Some(Value::Array(a)) = m.get("block") else { return Vec::new() };
    a.iter()
        .filter_map(|v| v.as_str())
        .map(|s| s.chars().filter(|c| (' '..='~').contains(c)).take(24).collect::<String>().trim().to_string())
        .filter(|s| !s.is_empty())
        .take(BLOCK_MAX)
        .collect()
}

/// a connection's first hello: its player's name, and whether it takes the changes to the peer list, bundles, and has
/// LAN play open (a page of before says nothing: it gets every broadcast)
fn greet(peer: &Peer, m: &Map<String, Value>, fpred: bool) {
    use nfsu2_util::json::is_num;
    peer.hello.store(true, Relaxed);
    peer.deltas.store(is_num(m.get("deltas"), 1.0), Relaxed);
    peer.bundle.store(is_num(m.get("bundle"), 1.0), Relaxed);
    use crate::conn::{COMPACT, DELTAS, FPRED, PLAIN};
    let compact = match (is_num(m.get("compact"), 1.0), is_num(m.get("delta"), 1.0), is_num(m.get("fpred"), 1.0)) {
        (false, ..) => PLAIN,
        (true, false, _) => COMPACT,
        (true, true, false) => DELTAS,
        (true, true, true) => FPRED,
    };
    let compact = if compact == FPRED && !fpred { DELTAS } else { compact };
    peer.conn.compact.store(compact, Relaxed);
    let lan = m.get("lan");
    if is_num(lan, 0.0) || is_num(lan, 1.0) {
        peer.lan.store(if is_num(lan, 1.0) { LAN_ON } else { LAN_OFF }, Relaxed);
    }
    if let Some(Value::String(name)) = m.get("name") {
        let _ = peer.name.set(crate::clean_name(name).into_boxed_str());
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::LanOptions;

    // a hello's "block": strings only, trimmed, printable ASCII, at most 24 characters each and BLOCK_MAX names
    #[test]
    fn a_hellos_block_is_names_as_the_game_shows_them() {
        let m = |v: Value| block_names(v.as_object().unwrap());
        assert_eq!(m(serde_json::json!({ "t": "hello" })), Vec::<String>::new());
        assert_eq!(m(serde_json::json!({ "block": "bob" })), Vec::<String>::new());
        let long = "x".repeat(40);
        assert_eq!(
            m(serde_json::json!({ "block": ["  Bob ", 7, "", "   ", "Zoë\u{0}!", long] })),
            vec!["Bob".to_string(), "Zo!".to_string(), "x".repeat(24)]
        );
        let many: Vec<String> = (0..40).map(|i| format!("p{i}")).collect();
        assert_eq!(m(serde_json::json!({ "block": many })).len(), BLOCK_MAX);
    }

    /// Node sent the welcome (and a resume's whole list) before it went back to its event loop: no frame from another
    /// player could come first.  Here the other players' readers route on other cores: a newcomer they can find (the
    /// table, the list of all) must have its welcome queued already.
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn the_welcome_is_queued_before_anyone_can_send_to_the_newcomer() {
        const N: usize = 300;
        let sw = LanSwitch::new(LanOptions { max_peers: 6000, ..LanOptions::default() }, "", None, None, None, None);
        // a sender on another core: it looks at each address the switch hands out next, as soon as it is taken
        let watcher = {
            let sw = sw.clone();
            std::thread::spawn(move || {
                let mut early = 0;
                for i in 0..N {
                    let p = loop {
                        if let Some(p) = sw.table[slot(vip_at(i)).unwrap()].load_full() {
                            break p;
                        }
                        std::hint::spin_loop();
                    };
                    let q = p.conn.out.q.lock();
                    if !matches!(q.first(), Some(Out::Text(t)) if t.starts_with(br#"{"t":"welcome""#)) {
                        early += 1;
                    }
                }
                early
            })
        };
        let peers: Vec<_> = (0..N).map(|i| crate::route::tests::peer(&sw, &format!("g{i}"))).collect();
        let early = watcher.join().unwrap();
        assert_eq!(early, 0, "{early} of {N} newcomers could be sent to before their welcome was queued");
        drop(peers);
        sw.stop();
    }

    /// what `full()` costs (under the members lock) at 1000 players: the start page asks for it with /players
    #[tokio::test]
    #[ignore]
    async fn what_full_costs() {
        let sw = LanSwitch::new(LanOptions { max_peers: 6000, ..LanOptions::default() }, "", None, None, None, None);
        let peers: Vec<_> = (0..1000).map(|i| crate::route::tests::peer(&sw, &format!("g{i}"))).collect();
        let t = std::time::Instant::now();
        let mut n = 0;
        for _ in 0..10_000 {
            n += sw.full() as usize;
        }
        println!("full() at 1000 players: {:.2} us a call ({n} full)", t.elapsed().as_secs_f64() * 1e6 / 10_000.0);
        drop(peers);
        sw.stop();
    }
}
