//! A connection's reader: a resume's wait for its hello, then its messages (text, binary, ping, pong, close), each
//! frame routed here, on the reader's own core.

use std::sync::Arc;
use std::sync::atomic::Ordering::Relaxed;
use std::time::Duration;

use bytes::Bytes;
use nfsu2_util::{mono_ms, now_ms};
use nfsu2_ws::{Message, ReadError, WsReader};
use serde_json::{Map, Value};
use tokio::io::{AsyncRead, AsyncReadExt};
use tokio::time::Instant;

use crate::compact::KIND_COMPACT;
use crate::conn::{Conn, Out};
use crate::text::parse_json;
use crate::{AttachCtx, Cut, KIND_BUNDLE, LanSwitch, Peer};

pub(crate) async fn read_loop<R>(sw: Arc<LanSwitch>, conn: Arc<Conn>, mut r: R, pre: Bytes, ctx: Arc<AttachCtx>)
where
    R: AsyncRead + Unpin + Send,
{
    let mut peer = conn.peer.lock().clone();
    // a resume: the hello first (the welcome follows it), within `resume_wait_ms`
    let mut wait = (ctx.resume && peer.is_none()).then(|| Instant::now() + Duration::from_millis(sw.o.resume_wait_ms));
    let mut ws = WsReader::new(&mut r, pre, sw.o.max_payload);
    // one wait for the kill token for the connection's life (a new one each message registered with the token and
    // left it again: two locks a message)
    let killed = conn.kill.cancelled();
    tokio::pin!(killed);
    // its first control message, the hello, names its page's id: a banned one goes (the crate doc's Kicks and bans)
    let mut first_text = true;
    loop {
        // (a page that waited in line has its peer once the line let it in: queue.rs)
        if peer.is_none() && wait.is_none() {
            peer = conn.peer.lock().clone();
        }
        let m = {
            let next = ws.next();
            tokio::pin!(next);
            loop {
                tokio::select! {
                    biased;
                    _ = &mut killed => return,
                    m = &mut next => break m,
                    _ = tokio::time::sleep_until(wait.unwrap_or_else(Instant::now)), if wait.is_some() => {
                        // no hello in time: a fresh address (and the message being read is an ordinary one)
                        wait = None;
                        peer = sw.admit(&conn, &ctx, None);
                    }
                }
            }
        };
        // (a connection that was cut is no longer open: what it sent last goes nowhere)
        match m {
            Ok(Message::Text(b)) => {
                if std::mem::take(&mut first_text) && sw.hello_refused(&conn, &ctx, &b) {
                    continue;
                }
                if wait.take().is_some() {
                    peer = sw.admit(&conn, &ctx, first_hello(&sw, &b));
                } else if let Some(p) = peer.as_ref().filter(|_| conn.open()) {
                    sw.on_message(p, true, b);
                } else if peer.is_none() && conn.open() && !sw.waiting_text(&conn, &b) {
                    // let in just now: the player's message
                    peer = conn.peer.lock().clone();
                    if let Some(p) = &peer {
                        sw.on_message(p, true, b);
                    }
                }
            }
            Ok(Message::Binary(b)) => {
                if wait.take().is_some() {
                    peer = sw.admit(&conn, &ctx, None);
                } else if let Some(p) = peer.as_ref().filter(|_| conn.open()) {
                    sw.on_message(p, false, b);
                    // the Online host behind (nfsu2_util::Backlog): what this client sends it waits in its socket.  (A
                    // load first, not a swap: free roam's messages never feed the host, and a swap is a locked write
                    // on each of them; only this reader sets and clears the flag.)
                    if p.fed_online.load(Relaxed) {
                        p.fed_online.store(false, Relaxed);
                        if sw.online_backlog.over() {
                            tokio::select! {
                                biased;
                                _ = &mut killed => return,
                                _ = sw.online_backlog.room() => {}
                            }
                        }
                    }
                }
            }
            Ok(Message::Ping(d)) => {
                // answered here, under the rate and while the queue has room (nfsu2-ws answers none itself): an answer
                // to every ping queues pongs without a limit for a client that never reads (the phase-2 deploy
                // review's finding 1, of the ws library's own answer: gigabytes in seconds); a copy, not the block the
                // ping was read into (the crate doc's one copy)
                if peer.as_ref().filter(|_| conn.open()).is_some_and(|p| !sw.flood(p) && sw.room(p, Cut::Slow)) {
                    conn.push(Out::Pong(Bytes::copy_from_slice(&d)));
                }
            }
            Ok(Message::Pong(d)) => {
                // only a pong carrying the payload of one of the switch's pings counts: its round trip is timed, and
                // for a ping queued since the keepalive's last tick it says the client is there (lag.rs)
                if let Some(p) = &peer {
                    let ours = p.probe.lock().pong(&d, nfsu2_util::mono_ms(), &p.share);
                    match ours {
                        Some(true) => p.alive.store(true, Relaxed),
                        Some(false) => {}
                        None => {
                            sw.flood(p);
                        }
                    }
                }
            }
            Ok(Message::Close(code, reason)) => {
                conn.close_received(code, reason);
                break;
            }
            Err(ReadError::Protocol { code, .. }) => {
                conn.protocol_error(code);
                break;
            }
            Err(ReadError::Closed) => return,
        }
    }
    // after a close frame (or a broken one) nothing more is read as messages: what comes is drained until the client
    // ends the connection, or the close timer does
    drop(ws);
    let mut scratch = vec![0u8; 4096];
    loop {
        tokio::select! {
            biased;
            _ = &mut killed => return,
            n = r.read(&mut scratch) => match n {
                Ok(0) | Err(_) => return,
                Ok(_) => {}
            },
        }
    }
}

/// a resume's first message: a hello, when it is one
fn first_hello(sw: &LanSwitch, b: &Bytes) -> Option<Map<String, Value>> {
    if b.len() > sw.o.max_text {
        return None;
    }
    match parse_json(b) {
        Some(Value::Object(m)) if m.get("t").and_then(Value::as_str) == Some("hello") => Some(m),
        _ => None,
    }
}

impl LanSwitch {
    /// a control frame over `ctl_rate` cuts the connection (no browser sends any)
    pub(crate) fn flood(&self, p: &Peer) -> bool {
        if p.ctl.lock().ctl.take_at(mono_ms(), 1.0) {
            return false;
        }
        self.cut(p, Cut::Flood);
        true
    }

    /// a message of an admitted connection: a control message (`text_rate`, `max_text`), a bundle (`max_bundle`) or
    /// a frame (`max_frame`)
    fn on_message(&self, peer: &Arc<Peer>, text: bool, data: Bytes) {
        peer.alive.store(true, Relaxed);
        peer.last_in.store(now_ms(), Relaxed);
        let n = data.len();
        let o = &self.o;
        if text {
            if n > o.max_text || !peer.ctl.lock().texts.take_at(mono_ms(), 1.0) {
                self.stats.limited.inc();
                return;
            }
            let Some(Value::Object(m)) = parse_json(&data) else { return };
            match m.get("t").and_then(Value::as_str) {
                Some("hello") => self.hello(peer, &m),
                Some("lan") => {
                    // its sockets' ports first: a broadcast routed after the list changed sees them
                    peer.udp.store(crate::udp_of(m.get("udp")), Relaxed);
                    self.set_lan(peer, nfsu2_util::json::is_num(m.get("on"), 1.0))
                }
                Some("ping") => {
                    if let Some(id) = m.get("id").and_then(nfsu2_util::json::safe_integer) {
                        self.send(peer, Out::Text(Bytes::from(format!(r#"{{"t":"pong","id":{id}}}"#))));
                    }
                }
                Some("friend" | "room") => self.page_text(peer, m.clone()),
                _ => {}
            }
            return;
        }
        if n < 12 {
            return;
        }
        if data[0] == KIND_BUNDLE {
            if n > o.max_bundle {
                self.stats.limited.inc();
                return;
            }
            self.stats.bundles_in.inc();
            // each entry as a frame of its own; a broken entry, or one over the budgets, ends the bundle
            let mut at = 4;
            while at + 4 <= n {
                let k = data[at] as usize;
                let len = u16::from_be_bytes([data[at + 2], data[at + 3]]) as usize;
                let f = at + 4 + 4 * k;
                let end = f + len;
                if len < 12 || len > o.max_frame || end > n || data[f] == KIND_BUNDLE || data[f] == KIND_COMPACT {
                    self.stats.limited.inc();
                    return;
                }
                let frame = data.slice(f..end);
                let routed = if k == 0 {
                    self.route(peer, &frame, std::iter::once(be32(&frame, 8)))
                } else {
                    self.route(peer, &frame, (0..k).map(|j| be32(&data, at + 4 + 4 * j)))
                };
                if !routed {
                    return;
                }
                at = end;
            }
            return;
        }
        // (a compact bundle is the switch's own: the page takes its sources as they are)
        if n > o.max_frame || data[0] == KIND_COMPACT {
            self.stats.limited.inc();
            return;
        }
        self.route(peer, &data, std::iter::once(be32(&data, 8)));
    }
}

fn be32(b: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}
