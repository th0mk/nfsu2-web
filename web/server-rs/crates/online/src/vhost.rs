//! The server's own host on the virtual LAN: 10.77.0.1, the address the switch never hands to a browser.
//! The game's online mode connects here (the runtime resolves pcnfs05.ea.com to it, winsock.c), so this is the one
//! place where the server itself speaks the browsers' virtual-LAN frames instead of routing them:
//!
//! - kind 4 (TCP): a stream endpoint: SYN to a port something listens on -> SYNACK, else RST; DATA in order; FIN;
//!   RST.  The WebSocket and the switch are reliable and ordered (no retransmission, as between browsers:
//!   runtime/web/net_lan.c)
//! - kind 5 (ICMP): echo requests answered (the game's ProtoPing shows the server's round trip)
//! - kind 3 (UDP): nothing listens: "port unreachable", as a host would answer
//!
//! Frame layout (big-endian, net_proto.h): [0] kind, [4..7] source vip (the switch wrote the sender's own), [8..11]
//! destination vip, [12..13] source port, [14..15] destination port, then the 8-byte LAN header ([16] op, [17] ttl,
//! [20..23] connection id little-endian) and the data.
//!
//! A connection is named by the browser's address, its port and its random connection id.  The address is the one
//! the switch wrote, so a browser can only ever speak for itself.  Limits: `max_conns` streams at once in all,
//! `per_vip` per browser (more SYNs are refused with RST).  A connection also knows the opaque group and site ids of
//! the client that opened it (the switch's: one client address, one IPv6 /48), for the services' per-client limits.
//! What the switch will not take (its receiver is too far behind: nfsu2-lan `online_queue`) ends the connection: a
//! stream with a hole in it is no use to the game.
//!
//! A connection's data, its end and its close go to the service that owns it ([`Svc`]); a connection that is gone
//! is no longer in the map, so whatever a service does with it after that (write, end, destroy) does nothing.

use std::collections::{BTreeMap, HashMap};
use std::sync::Arc;

use nfsu2_util::{FrameCtx, SendResult};

use crate::core::Core;
use crate::{VHOST_VIP, http};

pub type ConnId = u64;

/// a connection's browser, its address as text, its client's and site's ids
pub type ConnIds = (u32, String, Arc<str>, Option<Arc<str>>);

const K_UDP: u8 = 3;
const K_TCP: u8 = 4;
const K_ICMP: u8 = 5;
pub const SYN: u8 = 1;
pub const SYNACK: u8 = 2;
pub const RST: u8 = 3;
pub const DATA: u8 = 4;
pub const FIN: u8 = 5;
const UDP_UNREACH: u8 = 1;
const LAN_HDR: usize = 8;
const F_DATA: usize = 16;
/// the most data one TCP frame carries (net_proto.h NET_MAX_PAYLOAD - NET_LAN_HDR)
pub const TCP_CHUNK: usize = 1472 - LAN_HDR;

/// The virtual host's limits.  max_conns: a player in the lobby holds two (the lobby and EA Messenger), so 3000 lobby
/// sessions take 6000
#[derive(Clone, Copy, Debug)]
pub struct VhostLimits {
    pub max_conns: usize,
    pub per_vip: usize,
}

impl Default for VhostLimits {
    fn default() -> Self {
        VhostLimits { max_conns: 12000, per_vip: 8 }
    }
}

/// what listens on a port
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Listener {
    Redirector,
    Lobby,
    Buddy,
    Http,
    /// (tests) a service that records what it is told, ends its side when the browser ends its own
    #[cfg(test)]
    Probe,
    /// (tests) a service that takes connections and sets no handlers
    #[cfg(test)]
    Silent,
}

/// the service a connection belongs to (who gets its data, its end and its close)
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Svc {
    None,
    Redirector,
    Lobby(u64),
    Buddy(u64),
    Http,
    #[cfg(test)]
    Probe,
}

/// one stream between a browser and a port of the virtual host
pub struct VConn {
    pub vip: u32,
    pub rport: u16,
    pub lport: u16,
    pub cid: u32,
    /// the client's ids (a test without a switch: the browser's own address stands for its client)
    pub group: Arc<str>,
    pub site: Option<Arc<str>>,
    /// the keyphrase its page plays with (FrameCtx::owner, as of its first frame)
    pub owner: Option<Arc<str>>,
    pub rx_fin: bool,
    pub tx_fin: bool,
    pub bytes_in: u64,
    pub bytes_out: u64,
    pub svc: Svc,
}

impl VConn {
    /// "10.77.0.2"
    pub fn peer(&self) -> String {
        nfsu2_util::vip_str(self.vip)
    }
}

/// the virtual host's counts (the server's info: core.rs `Core::info`)
#[derive(Clone, Copy, Debug, Default)]
pub struct VhStats {
    pub syn_in: u64,
    pub refused: u64,
    pub limited: u64,
    pub rst_in: u64,
    pub frames_in: u64,
    pub frames_out: u64,
    pub echoes: u64,
    pub udp_unreach: u64,
    pub bad: u64,
    pub slow: u64,
    pub overrun: u64,
}

/// the redirector's state of one connection (lobby.rs `redirector_connect`)
pub struct Redir {
    pub reader: crate::aries::FrameReader,
    pub timer: Option<crate::timers::TimerId>,
}

pub struct VHost {
    pub o: VhostLimits,
    pub listeners: HashMap<u16, Listener>,
    /// the open connections, in the order they were opened (a browser that left has its streams closed in that order)
    pub conns: BTreeMap<ConnId, VConn>,
    by_key: HashMap<(u32, u16, u32), ConnId>,
    per_vip: HashMap<u32, usize>,
    next: ConnId,
    pub stats: VhStats,
    pub redirs: HashMap<ConnId, Redir>,
    pub https: HashMap<ConnId, http::HttpConn>,
}

impl VHost {
    pub fn new(o: VhostLimits) -> VHost {
        VHost {
            o,
            listeners: HashMap::new(),
            conns: BTreeMap::new(),
            by_key: HashMap::new(),
            per_vip: HashMap::new(),
            next: 0,
            stats: VhStats::default(),
            redirs: HashMap::new(),
            https: HashMap::new(),
        }
    }
    pub fn listen(&mut self, port: u16, l: Listener) {
        self.listeners.insert(port, l);
    }
}

/// one switch frame of a LAN kind
#[allow(clippy::too_many_arguments)]
pub fn lan_frame(kind: u8, src: u32, dst: u32, sport: u16, dport: u16, op: u8, cid: u32, data: &[u8]) -> Vec<u8> {
    let mut f = vec![0u8; F_DATA + LAN_HDR + data.len()];
    f[0] = kind;
    f[4..8].copy_from_slice(&src.to_be_bytes());
    f[8..12].copy_from_slice(&dst.to_be_bytes());
    f[12..14].copy_from_slice(&sport.to_be_bytes());
    f[14..16].copy_from_slice(&dport.to_be_bytes());
    f[F_DATA] = op;
    f[F_DATA + 1] = 64;
    f[F_DATA + 4..F_DATA + 8].copy_from_slice(&cid.to_le_bytes());
    f[F_DATA + LAN_HDR..].copy_from_slice(data);
    f
}

fn icmp_checksum(m: &mut [u8]) {
    m[2] = 0;
    m[3] = 0;
    let mut sum: u32 = 0;
    let mut i = 0;
    while i + 1 < m.len() {
        sum += ((m[i] as u32) << 8) | m[i + 1] as u32;
        i += 2;
    }
    if m.len() & 1 == 1 {
        sum += (m[m.len() - 1] as u32) << 8;
    }
    while sum >> 16 != 0 {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    let sum = !sum & 0xffff;
    m[2] = (sum >> 8) as u8;
    m[3] = sum as u8;
}

fn be16(d: &[u8], at: usize) -> u16 {
    u16::from_be_bytes([d[at], d[at + 1]])
}
fn le32(d: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([d[at], d[at + 1], d[at + 2], d[at + 3]])
}

impl Core {
    /// a frame to one browser through the switch (the tap sees it first)
    pub fn out(&mut self, vip: u32, frame: Vec<u8>) -> SendResult {
        self.vh.stats.frames_out += 1;
        if let Some(t) = &self.tap {
            t(&frame);
        }
        match &self.send {
            Some(send) => send(vip, frame.into(), self.cause.as_deref()),
            // until the switch is attached, nothing is taken (as by a browser too far behind)
            None => SendResult::Full,
        }
    }

    /// a frame from a browser to 10.77.0.1 (data: the switch frame, source already the sender's own)
    pub fn vh_frame(&mut self, from: u32, data: &[u8], ctx: &FrameCtx) {
        self.vh.stats.frames_in += 1;
        if data.len() < F_DATA + LAN_HDR {
            self.vh.stats.bad += 1;
            return;
        }
        let (kind, sport, dport, op) = (data[0], be16(data, 12), be16(data, 14), data[F_DATA]);
        let cid = le32(data, F_DATA + 4);
        let payload = &data[F_DATA + LAN_HDR..];
        if kind == K_ICMP {
            // only echo requests get an answer
            if payload.len() < 8 || payload[0] != 8 {
                return;
            }
            let mut rep = payload.to_vec();
            rep[0] = 0;
            rep[1] = 0;
            icmp_checksum(&mut rep);
            self.vh.stats.echoes += 1;
            self.out(from, lan_frame(K_ICMP, VHOST_VIP, from, 0, 0, 0, 0, &rep));
            return;
        }
        if kind == K_UDP {
            if op == UDP_UNREACH {
                return;
            }
            self.vh.stats.udp_unreach += 1;
            self.out(from, lan_frame(K_UDP, VHOST_VIP, from, dport, sport, UDP_UNREACH, 0, &[]));
            return;
        }
        if kind != K_TCP {
            self.vh.stats.bad += 1;
            return;
        }
        let c = self.vh.by_key.get(&(from, sport, cid)).copied();
        let rst = |core: &mut Core| {
            core.out(from, lan_frame(K_TCP, VHOST_VIP, from, dport, sport, RST, cid, &[]));
        };
        match op {
            SYN => {
                // a duplicate
                if c.is_some() {
                    return;
                }
                self.vh.stats.syn_in += 1;
                let Some(l) = self.vh.listeners.get(&dport).copied() else {
                    self.vh.stats.refused += 1;
                    return rst(self);
                };
                if self.vh.conns.len() >= self.vh.o.max_conns || self.vh.per_vip.get(&from).copied().unwrap_or(0) >= self.vh.o.per_vip {
                    self.vh.stats.limited += 1;
                    return rst(self);
                }
                self.vh.next += 1;
                let id = self.vh.next;
                self.vh.conns.insert(
                    id,
                    VConn {
                        vip: from,
                        rport: sport,
                        lport: dport,
                        cid,
                        group: ctx.group.clone(),
                        site: ctx.site.clone(),
                        owner: ctx.owner.clone(),
                        rx_fin: false,
                        tx_fin: false,
                        bytes_in: 0,
                        bytes_out: 0,
                        svc: Svc::None,
                    },
                );
                self.vh.by_key.insert((from, sport, cid), id);
                *self.vh.per_vip.entry(from).or_insert(0) += 1;
                self.out(from, lan_frame(K_TCP, VHOST_VIP, from, dport, sport, SYNACK, cid, &[]));
                self.on_connect(l, id);
            }
            DATA => {
                let Some(id) = c else { return rst(self) };
                let conn = self.vh.conns.get_mut(&id).expect("indexed");
                if conn.rx_fin {
                    return;
                }
                conn.bytes_in += payload.len() as u64;
                if !payload.is_empty() {
                    self.on_data(id, payload);
                }
            }
            FIN => {
                let Some(id) = c else { return };
                let conn = self.vh.conns.get_mut(&id).expect("indexed");
                if conn.rx_fin {
                    return;
                }
                conn.rx_fin = true;
                self.on_end(id);
                if self.vh.conns.get(&id).is_some_and(|c| c.tx_fin) {
                    self.conn_drop(id, "closed");
                }
            }
            RST => {
                if let Some(id) = c {
                    self.vh.stats.rst_in += 1;
                    self.conn_drop(id, "reset by the game");
                }
            }
            // we never connect anywhere
            SYNACK => {
                if c.is_none() {
                    rst(self);
                }
            }
            _ => self.vh.stats.bad += 1,
        }
    }

    /// bytes to the browser, in frames of at most TCP_CHUNK; one the switch does not take ends
    /// the connection (Full: the browser is too far behind; Gone: it left the switch)
    pub fn conn_write(&mut self, id: ConnId, buf: &[u8]) -> bool {
        let Some(c) = self.vh.conns.get(&id) else { return false };
        if c.tx_fin {
            return false;
        }
        let (vip, lport, rport, cid) = (c.vip, c.lport, c.rport, c.cid);
        for part in buf.chunks(TCP_CHUNK) {
            let sent = self.out(vip, lan_frame(K_TCP, VHOST_VIP, vip, lport, rport, DATA, cid, part));
            if !sent.sent() {
                if sent == SendResult::Full {
                    self.vh.stats.slow += 1;
                }
                self.conn_destroy(id, if sent == SendResult::Full { "the browser does not keep up" } else { "the browser left" });
                return false;
            }
        }
        if let Some(c) = self.vh.conns.get_mut(&id) {
            c.bytes_out += buf.len() as u64;
        }
        true
    }

    /// graceful close of our side (FIN); the connection goes once both sides closed
    pub fn conn_end(&mut self, id: ConnId) {
        let Some(c) = self.vh.conns.get_mut(&id) else { return };
        if c.tx_fin {
            return;
        }
        c.tx_fin = true;
        let (vip, lport, rport, cid, rx_fin) = (c.vip, c.lport, c.rport, c.cid, c.rx_fin);
        self.out(vip, lan_frame(K_TCP, VHOST_VIP, vip, lport, rport, FIN, cid, &[]));
        if rx_fin {
            self.conn_drop(id, "closed");
        }
    }

    /// abortive close (RST)
    pub fn conn_destroy(&mut self, id: ConnId, why: &str) {
        let Some(c) = self.vh.conns.get(&id) else { return };
        let (vip, lport, rport, cid) = (c.vip, c.lport, c.rport, c.cid);
        self.out(vip, lan_frame(K_TCP, VHOST_VIP, vip, lport, rport, RST, cid, &[]));
        self.conn_drop(id, why);
    }

    /// the connection is gone; its service hears why
    pub fn conn_drop(&mut self, id: ConnId, why: &str) {
        let Some(c) = self.vh.conns.remove(&id) else { return };
        self.vh.by_key.remove(&(c.vip, c.rport, c.cid));
        let n = self.vh.per_vip.get(&c.vip).copied().unwrap_or(1).saturating_sub(1);
        if n > 0 {
            self.vh.per_vip.insert(c.vip, n);
        } else {
            self.vh.per_vip.remove(&c.vip);
        }
        self.on_close(c.svc, id, why);
    }

    /// what this host sent because of frame `data` from `from` was more than that client's budgets: the stream it
    /// came on goes (the budgets are the switch's: nfsu2-lan)
    pub fn overrun(&mut self, from: u32, data: &[u8]) {
        if data.len() < F_DATA + LAN_HDR || data[0] != K_TCP {
            return;
        }
        if let Some(id) = self.vh.by_key.get(&(from, be16(data, 12), le32(data, F_DATA + 4))).copied() {
            self.vh.stats.overrun += 1;
            self.conn_destroy(id, "it made the server send others more than its share");
        }
    }

    /// a handler failed on this frame (it panicked): the stream it came on goes
    pub fn fail_stream(&mut self, from: u32, data: &[u8]) {
        if data.len() < F_DATA + LAN_HDR || data[0] != K_TCP {
            return;
        }
        if let Some(id) = self.vh.by_key.get(&(from, be16(data, 12), le32(data, F_DATA + 4))).copied() {
            self.conn_destroy(id, "service error");
        }
    }

    /// a browser left the switch: its streams are gone
    pub fn vh_peer_gone(&mut self, vip: u32) {
        let ids: Vec<ConnId> = self.vh.conns.iter().filter(|(_, c)| c.vip == vip).map(|(id, _)| *id).collect();
        for id in ids {
            self.conn_drop(id, "the browser left");
        }
    }

    // ---- the services' callbacks ----

    fn on_connect(&mut self, l: Listener, id: ConnId) {
        match l {
            Listener::Redirector => self.redirector_connect(id),
            Listener::Lobby => self.lobby_connect(id),
            Listener::Buddy => self.buddy_connect(id),
            Listener::Http => self.http_connect(id),
            #[cfg(test)]
            Listener::Probe => self.set_svc(id, Svc::Probe),
            #[cfg(test)]
            Listener::Silent => {}
        }
    }

    pub fn set_svc(&mut self, id: ConnId, svc: Svc) {
        if let Some(c) = self.vh.conns.get_mut(&id) {
            c.svc = svc;
        }
    }

    fn on_data(&mut self, id: ConnId, payload: &[u8]) {
        let Some(svc) = self.vh.conns.get(&id).map(|c| c.svc) else { return };
        match svc {
            Svc::None => {}
            Svc::Redirector => self.redirector_data(id, payload),
            Svc::Lobby(s) => self.lobby_data(s, payload),
            Svc::Buddy(b) => self.buddy_data(b, payload),
            Svc::Http => self.http_data(id, payload),
            #[cfg(test)]
            Svc::Probe => self.probe.push(format!("data {}", String::from_utf8_lossy(payload))),
        }
    }

    fn on_end(&mut self, id: ConnId) {
        let Some(svc) = self.vh.conns.get(&id).map(|c| c.svc) else { return };
        match svc {
            Svc::None => {}
            Svc::Redirector => self.conn_end(id),
            Svc::Lobby(s) => {
                self.lobby_leave(s, "closed by the game");
                self.conn_end(id);
            }
            Svc::Buddy(b) => {
                self.buddy_gone(b);
                self.conn_end(id);
            }
            Svc::Http => {
                if !self.vh.https.get(&id).is_some_and(|h| h.done) {
                    self.conn_end(id);
                }
            }
            #[cfg(test)]
            Svc::Probe => {
                self.probe.push("end".into());
                self.conn_end(id);
            }
        }
    }

    fn on_close(&mut self, svc: Svc, id: ConnId, why: &str) {
        match svc {
            Svc::None => {}
            Svc::Redirector => {
                if let Some(t) = self.vh.redirs.remove(&id).and_then(|r| r.timer) {
                    self.timers.clear(t);
                }
            }
            Svc::Lobby(s) => self.lobby_leave(s, why),
            Svc::Buddy(b) => self.buddy_closed(b),
            Svc::Http => {
                if let Some(t) = self.vh.https.remove(&id).and_then(|h| h.timer) {
                    self.timers.clear(t);
                }
            }
            #[cfg(test)]
            Svc::Probe => self.probe.push(format!("close {why}")),
        }
        let _ = why;
    }

    /// the connection's peer address, group and site
    pub fn conn_ids(&self, id: ConnId) -> Option<ConnIds> {
        self.vh.conns.get(&id).map(|c| (c.vip, c.peer(), c.group.clone(), c.site.clone()))
    }

    /// the keyphrase the connection's page plays with (nfsu2-claims' owner), if it named one
    pub fn conn_owner(&self, id: ConnId) -> Option<Arc<str>> {
        self.vh.conns.get(&id).and_then(|c| c.owner.clone())
    }

    /// the connection is still there (not closed, reset or gone with its browser)
    pub fn conn_open(&self, id: ConnId) -> bool {
        self.vh.conns.contains_key(&id)
    }
}
