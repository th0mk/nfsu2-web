//! The switch's test harness: `start` and `start_full` (a switch behind a listener on 127.0.0.1:0 that answers the
//! upgrade to /lan and attaches with ?resume, ?queue, ?g= and ?s=), the fake browser `Client` (tokio-tungstenite),
//! `raw_ws` and `raw_peer` (plain sockets that read only when told to), `frame_of`, `bundle_of`, `throttle`.  The listener also
//! answers GET /state with what the page tests read of the switch (they run in node: tests/page.mjs), and GET
//! /expel?vip=&code=&msg= with the operator's kick or ban of that player.
#![allow(dead_code)]

use std::collections::HashSet;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use bytes::Bytes;
use futures_util::{SinkExt, StreamExt};
use indexmap::IndexMap;
use nfsu2_lan::{AttachOpts, LanOptions, LanSwitch, PeerView};
use nfsu2_util::Host;
use serde_json::{Map, Value, json};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpSocket, TcpStream};
use tokio::sync::{mpsc, oneshot, watch};
use tokio::task::JoinHandle;
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::tungstenite::protocol::CloseFrame;

pub const BROADCAST: u32 = 0xFFFF_FFFF;

/// The tests run one at a time: many of them measure time and queues.
pub async fn serial() -> tokio::sync::MutexGuard<'static, ()> {
    static M: tokio::sync::Mutex<()> = tokio::sync::Mutex::const_new(());
    M.lock().await
}

pub async fn sleep(ms: u64) {
    tokio::time::sleep(Duration::from_millis(ms)).await;
}

pub fn ip(s: &str) -> u32 {
    s.split('.').fold(0u32, |a, b| (a << 8) | b.parse::<u32>().unwrap())
}

pub fn ip_str(v: u32) -> String {
    nfsu2_util::vip_str(v)
}

/// every address the switch hands out: host bytes 2-251 in each of 10.77.0-23.x (10.77.0.0/19)
pub fn all_vips() -> Vec<u32> {
    (0..24u32).flat_map(|b| (0..250u32).map(move |i| (10 << 24) | (77 << 16) | (b << 8) | (i + 2))).collect()
}

/// the tests' options: WebSocket messages of up to 1 MiB taken (max_payload), so that the switch's own limits decide
pub fn opts() -> LanOptions {
    LanOptions { max_payload: 1 << 20, ..LanOptions::default() }
}

pub struct Switch {
    pub lan: Arc<LanSwitch>,
    pub port: u16,
    accept: JoinHandle<()>,
}

pub async fn start(o: LanOptions) -> Switch {
    start_full(o, "", None, None, None).await
}

/// a switch on an ephemeral port, attached as the server's /lan attaches (src/app/lan.rs), with the group and site ids
/// the server derives from the client's address taken from ?g=<id> and ?s=<id> instead; SO_SNDBUF on its sockets as
/// the server sets it (send_buffer_os)
pub async fn start_full(
    o: LanOptions,
    impair: &str,
    online: Option<Arc<dyn Host>>,
    roam: Option<Arc<dyn Host>>,
    store: Option<std::path::PathBuf>,
) -> Switch {
    let sndbuf = o.send_buffer_os;
    let lan = LanSwitch::new(o, impair, online, roam, None, store);
    let sock = TcpSocket::new_v4().unwrap();
    if sndbuf > 0 {
        sock.set_send_buffer_size(sndbuf as u32).unwrap();
    }
    sock.bind("127.0.0.1:0".parse().unwrap()).unwrap();
    let listener = sock.listen(1024).unwrap();
    let port = listener.local_addr().unwrap().port();
    let l2 = lan.clone();
    let accept = tokio::spawn(async move {
        while let Ok((s, _)) = listener.accept().await {
            let _ = s.set_nodelay(true);
            tokio::spawn(serve(l2.clone(), s));
        }
    });
    Switch { lan, port, accept }
}

async fn serve(lan: Arc<LanSwitch>, mut s: TcpStream) {
    let mut buf = Vec::new();
    let mut tmp = [0u8; 4096];
    let end = loop {
        if let Some(i) = buf.windows(4).position(|w| w == b"\r\n\r\n") {
            break i + 4;
        }
        if buf.len() > 16 << 10 {
            return;
        }
        match s.read(&mut tmp).await {
            Ok(0) | Err(_) => return,
            Ok(n) => buf.extend_from_slice(&tmp[..n]),
        }
    };
    let head = String::from_utf8_lossy(&buf[..end]).to_string();
    let mut lines = head.split("\r\n");
    let target = lines.next().and_then(|l| l.split(' ').nth(1)).unwrap_or("").to_string();
    let key = lines
        .filter_map(|l| l.split_once(':'))
        .find(|(k, _)| k.trim().eq_ignore_ascii_case("sec-websocket-key"))
        .map(|(_, v)| v.trim().to_string());
    let (path, query) = target.split_once('?').unwrap_or((&target, ""));
    // the operator's kick (code 4001) or ban (4003) of vip, with msg: the page tests remove their page this way
    if path == "/expel" {
        let arg = |k: &str| query.split('&').find_map(|kv| kv.strip_prefix(&format!("{k}="))).unwrap_or("").to_string();
        let code: u16 = arg("code").parse().unwrap_or(4001);
        let m = json!({ "t": if code == 4001 { "kick" } else { "ban" }, "msg": arg("msg") });
        let done = lan.expel(arg("vip").parse().unwrap_or(0), Bytes::from(m.to_string()), code);
        let answer = format!("HTTP/1.1 {}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", if done { "200 OK" } else { "404 Not Found" });
        let _ = s.write_all(answer.as_bytes()).await;
        return;
    }
    if path == "/state" {
        let body = state(&lan).await.to_string();
        let answer = format!(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        );
        let _ = s.write_all(answer.as_bytes()).await;
        return;
    }
    let Some(key) = key.filter(|_| path == "/lan") else { return };
    if s.write_all(&nfsu2_ws::handshake_response(&key, None)).await.is_err() {
        return;
    }
    let (mut resume, mut queue) = (false, false);
    let (mut group, mut site) = (None, None);
    let (mut ident, mut refuse, mut gate) = (None, None, None);
    for kv in query.split('&').filter(|kv| !kv.is_empty()) {
        let (k, v) = kv.split_once('=').unwrap_or((kv, ""));
        match k {
            "resume" => resume = true,
            "queue" => queue = true,
            "g" if group.is_none() => group = Some(v.to_string()),
            "s" if site.is_none() => site = Some(v.to_string()),
            // the server's bans: its id for the client's address, a client it turns away at once, a page id it turns
            // away at the hello (with {"t":"ban","msg":<id>})
            "ident" => ident = Some(v.to_string()),
            "refuse" => refuse = Some(Bytes::from(json!({ "t": "ban", "msg": v }).to_string())),
            "gate" => gate = Some(v.to_string()),
            _ => {}
        }
    }
    let on_hello: Option<nfsu2_lan::OnHello> = gate.map(|g| {
        Box::new(move |id: &str| (id == g).then(|| Bytes::from(json!({ "t": "ban", "msg": id }).to_string()))) as nfsu2_lan::OnHello
    });
    let pre = Bytes::copy_from_slice(&buf[end..]);
    let (r, w) = s.into_split();
    lan.attach(r, w, pre, AttachOpts { resume, queue, group, site, ident, refuse, on_hello, ..AttachOpts::default() });
}

/// what the page tests read of the switch (sw.lan.peers, info(), reserved)
async fn state(lan: &LanSwitch) -> Value {
    let peers: Vec<Value> =
        lan.peers_view().iter().map(|p| json!({ "vip": p.vip, "name": p.name, "hello": p.hello, "lan": p.lan })).collect();
    json!({ "peers": peers, "info": lan.info().await, "reserved": lan.reserved_view().len() })
}

impl Switch {
    /// the switch stops, its connections end
    pub async fn close(self) {
        self.lan.stop();
        self.lan.terminate_all();
        self.accept.abort();
        sleep(20).await;
    }
    pub async fn info(&self) -> Value {
        self.lan.info().await
    }
    pub fn peers(&self) -> Vec<PeerView> {
        self.lan.peers_view()
    }
    pub fn peer_named(&self, name: &str) -> Option<PeerView> {
        self.peers().into_iter().find(|p| p.name == name)
    }
    pub fn has(&self, vip: u32) -> bool {
        self.peers().iter().any(|p| p.vip == vip)
    }
    pub fn reserved(&self) -> Vec<u32> {
        self.lan.reserved_view().iter().map(|r| r.vip).collect()
    }
}

/// `until(f, ms)` of the page tests: f polled every 20 ms
pub async fn wait_for(ms: u64, mut f: impl FnMut() -> bool) {
    let t0 = Instant::now();
    while !f() {
        assert!(t0.elapsed() < Duration::from_millis(ms), "timed out");
        sleep(20).await;
    }
}

/// a frame as `Client::udp` makes one
pub fn frame_of(dst: u32, data: &[u8], kind: u8, sport: u16, dport: u16) -> Vec<u8> {
    let mut f = vec![0u8; 16 + data.len()];
    f[0] = kind;
    f[8..12].copy_from_slice(&dst.to_be_bytes());
    f[12..14].copy_from_slice(&sport.to_be_bytes());
    f[14..16].copy_from_slice(&dport.to_be_bytes());
    f[16..].copy_from_slice(data);
    f
}

/// a bundle (kind 32, nfsu2_lan's KIND_BUNDLE) of entries (a frame, its destinations: none for the frame's own)
pub fn bundle_of(entries: &[(Vec<u8>, Vec<u32>)]) -> Vec<u8> {
    let mut b = vec![32, 0, 0, 0];
    for (f, to) in entries {
        b.push(to.len() as u8);
        b.push(0);
        b.extend_from_slice(&(f.len() as u16).to_be_bytes());
        for v in to {
            b.extend_from_slice(&v.to_be_bytes());
        }
        b.extend_from_slice(f);
    }
    b
}

/// a datagram a client got, the bundles taken apart
#[derive(Clone, Debug)]
pub struct Dgram {
    pub msg: usize,
    pub kind: u8,
    pub src: u32,
    pub dst: u32,
    pub data: String,
    pub buf: Vec<u8>,
}

fn be32(b: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

pub fn dgrams_of(frames: &[(Vec<u8>, Instant)]) -> Vec<Dgram> {
    let mut out = Vec::new();
    for (msg, (buf, _)) in frames.iter().enumerate() {
        let mut one = |b: &[u8]| {
            out.push(Dgram {
                msg,
                kind: b[0],
                src: be32(b, 4),
                dst: be32(b, 8),
                data: String::from_utf8_lossy(&b[16..]).to_string(),
                buf: b.to_vec(),
            })
        };
        if buf[0] != 32 {
            one(buf);
            continue;
        }
        let mut at = 4;
        while at + 4 <= buf.len() {
            let f = at + 4 + 4 * buf[at] as usize;
            let end = f + u16::from_be_bytes([buf[at + 2], buf[at + 3]]) as usize;
            one(&buf[f..end]);
            at = end;
        }
    }
    out
}

/// What a fake browser got.
#[derive(Default)]
pub struct Got {
    /// binary messages, as they came, and when
    pub frames: Vec<(Vec<u8>, Instant)>,
    /// control messages
    pub ctl: Vec<Value>,
    pub ctl_bytes: usize,
    /// the peer list as the whole lists and the changes made it
    pub view: Option<IndexMap<u32, Value>>,
    pub pings: usize,
    pub pongs: Vec<Vec<u8>>,
    /// its close: the code and reason of the switch's close frame (1005 without a code, 1006 without a frame)
    pub closed: Option<(u16, String)>,
}

impl Got {
    pub fn last_peers(&self) -> Option<Vec<Value>> {
        self.view.as_ref().map(|v| v.values().cloned().collect())
    }
    pub fn names(&self) -> String {
        let mut n: Vec<String> =
            self.last_peers().unwrap_or_default().iter().map(|p| p["name"].as_str().unwrap_or("").to_string()).collect();
        n.sort();
        n.join(",")
    }
    pub fn count(&self, t: &str) -> usize {
        self.ctl.iter().filter(|m| m["t"] == t).count()
    }
    pub fn dgrams(&self) -> Vec<Dgram> {
        dgrams_of(&self.frames)
    }
}

enum Cmd {
    Send(Message),
    Flush(oneshot::Sender<()>),
}

#[derive(Default, Clone, Copy)]
pub struct Opts {
    pub deltas: bool,
    pub lan: Option<u8>,
    pub bundle: bool,
    /// and compact bundles (kind 33): its frames come as they are in `frames`, `unpack_compact` takes them apart
    pub compact: bool,
    /// its page's id (the hello's "id")
    pub id: Option<&'static str>,
    /// the players its page blocks (the hello's "block")
    pub block: Option<&'static [&'static str]>,
}

pub fn deltas() -> Opts {
    Opts { deltas: true, ..Opts::default() }
}

pub fn lan(on: u8) -> Opts {
    Opts { lan: Some(on), ..Opts::default() }
}

pub fn bundles() -> Opts {
    Opts { bundle: true, ..Opts::default() }
}

/// A fake browser: collects control messages and frames; `resume` = (vip, token): reconnects as the page does
/// (/lan?resume, then a hello with the old address and the token before anything else); its hello says deltas,
/// lan and bundle as `Opts` say.
pub struct Client {
    pub name: String,
    pub vip: u32,
    pub token: String,
    st: Arc<Mutex<Got>>,
    ver: watch::Receiver<u64>,
    tx: mpsc::UnboundedSender<Cmd>,
    resume: bool,
    extra: Map<String, Value>,
    tasks: Vec<JoinHandle<()>>,
}

impl Client {
    pub async fn connect(port: u16, name: &str) -> Client {
        Client::new(port, name, "", None, Opts::default()).await
    }

    pub async fn with(port: u16, name: &str, query: &str, o: Opts) -> Client {
        Client::new(port, name, query, None, o).await
    }

    pub async fn new(port: u16, name: &str, query: &str, resume: Option<(u32, &str)>, o: Opts) -> Client {
        let mut extra = Map::new();
        if o.deltas {
            extra.insert("deltas".into(), 1.into());
        }
        if let Some(l) = o.lan {
            extra.insert("lan".into(), l.into());
        }
        if o.bundle {
            extra.insert("bundle".into(), 1.into());
        }
        if o.compact {
            extra.insert("compact".into(), 1.into());
        }
        if let Some(id) = o.id {
            extra.insert("id".into(), id.into());
        }
        if let Some(b) = o.block {
            extra.insert("block".into(), b.into());
        }
        let q = match (resume.is_some(), query.is_empty()) {
            (true, true) => "?resume".to_string(),
            (true, false) => format!("{query}&resume"),
            (false, _) => query.to_string(),
        };
        let (ws, _) = tokio_tungstenite::connect_async(format!("ws://127.0.0.1:{port}/lan{q}")).await.expect("connect");
        let (mut sink, mut stream) = ws.split();
        let st = Arc::new(Mutex::new(Got::default()));
        let (vtx, ver) = watch::channel(0u64);
        let (tx, mut rx) = mpsc::unbounded_channel::<Cmd>();
        let writer = tokio::spawn(async move {
            while let Some(c) = rx.recv().await {
                match c {
                    Cmd::Send(m) => {
                        if sink.send(m).await.is_err() {
                            break;
                        }
                    }
                    Cmd::Flush(done) => {
                        let _ = done.send(());
                    }
                }
            }
        });
        // once the connection is over the socket goes (ws's client ends it after the close handshake)
        let writer_end = writer.abort_handle();
        let s2 = st.clone();
        let reader = tokio::spawn(async move {
            let mut code = None;
            while let Some(m) = stream.next().await {
                let Ok(m) = m else { break };
                {
                    let mut g = s2.lock().unwrap();
                    match m {
                        Message::Binary(b) => g.frames.push((b.to_vec(), Instant::now())),
                        Message::Text(t) => {
                            let m: Value = serde_json::from_str(t.as_str()).expect("JSON from the switch");
                            g.ctl_bytes += t.len();
                            if m["t"] == "peers" {
                                g.view = Some(
                                    m["peers"].as_array().unwrap().iter().map(|p| (p["vip"].as_u64().unwrap() as u32, p.clone())).collect(),
                                );
                            }
                            if let Some(v) = g.view.as_mut().filter(|_| m["t"] == "delta") {
                                for x in m["gone"].as_array().unwrap() {
                                    v.shift_remove(&(x.as_u64().unwrap() as u32));
                                }
                                for p in m["peers"].as_array().unwrap() {
                                    v.insert(p["vip"].as_u64().unwrap() as u32, p.clone());
                                }
                            }
                            g.ctl.push(m);
                        }
                        Message::Ping(_) => g.pings += 1,
                        Message::Pong(p) => g.pongs.push(p.to_vec()),
                        Message::Close(f) => {
                            code = Some(f.map(|f| (u16::from(f.code), f.reason.to_string())).unwrap_or((1005, String::new())))
                        }
                        Message::Frame(_) => {}
                    }
                }
                vtx.send_modify(|v| *v += 1);
            }
            writer_end.abort();
            drop(stream);
            s2.lock().unwrap().closed = Some(code.unwrap_or((1006, String::new())));
            vtx.send_modify(|v| *v += 1);
        });
        let c = Client {
            name: name.to_string(),
            vip: 0,
            token: String::new(),
            st,
            ver,
            tx,
            resume: resume.is_some(),
            extra,
            tasks: vec![reader, writer],
        };
        if let Some((vip, token)) = resume {
            let mut h = Map::new();
            h.insert("t".into(), "hello".into());
            h.insert("name".into(), name.into());
            h.insert("vip".into(), ip_str(vip).into());
            h.insert("token".into(), token.into());
            h.extend(c.extra.clone());
            c.text(&Value::Object(h).to_string());
        }
        c
    }

    pub fn got(&self) -> MutexGuard<'_, Got> {
        self.st.lock().unwrap()
    }

    /// waits until f says yes (2 s at most)
    pub async fn until<T>(&self, f: impl FnMut(&Got) -> Option<T>) -> T {
        self.until_ms(2000, f).await
    }

    pub async fn until_ms<T>(&self, ms: u64, mut f: impl FnMut(&Got) -> Option<T>) -> T {
        let deadline = tokio::time::Instant::now() + Duration::from_millis(ms);
        let mut ver = self.ver.clone();
        loop {
            if let Some(v) = f(&self.got()) {
                return v;
            }
            match tokio::time::timeout_at(deadline, ver.changed()).await {
                Ok(Ok(())) => {}
                Ok(Err(_)) => {
                    if let Some(v) = f(&self.got()) {
                        return v;
                    }
                    panic!("{}: closed while waiting", self.name);
                }
                Err(_) => panic!("{}: timed out", self.name),
            }
        }
    }

    pub async fn until_true(&self, mut f: impl FnMut(&Got) -> bool) {
        self.until(|g| f(g).then_some(())).await
    }

    /// the welcome; then its hello (a resume sent it first)
    pub async fn welcome(&mut self) -> Value {
        let m = self.until(|g| g.ctl.iter().find(|m| m["t"] == "welcome").cloned()).await;
        self.vip = m["vip"].as_u64().unwrap() as u32;
        self.token = m["token"].as_str().unwrap().to_string();
        if !self.resume {
            let mut h = Map::new();
            h.insert("t".into(), "hello".into());
            h.insert("name".into(), self.name.clone().into());
            h.extend(self.extra.clone());
            self.text(&Value::Object(h).to_string());
        }
        m
    }

    pub fn text(&self, s: &str) {
        let _ = self.tx.send(Cmd::Send(Message::text(s)));
    }

    pub fn binary(&self, b: Vec<u8>) {
        let _ = self.tx.send(Cmd::Send(Message::binary(b)));
    }

    /// a WebSocket ping (ws `ping(data)`)
    pub fn ping(&self, data: Vec<u8>) {
        let _ = self.tx.send(Cmd::Send(Message::Ping(data.into())));
    }

    /// a datagram with ports (kind 1 free roam, 2 the loopback, 3-5 LAN play), from `fake_src` as it claims
    pub fn udp(&self, dst: u32, sport: u16, dport: u16, data: &[u8], kind: u8, fake_src: u32) {
        let mut f = frame_of(dst, data, kind, sport, dport);
        f[4..8].copy_from_slice(&fake_src.to_be_bytes());
        self.binary(f);
    }

    pub fn send_bundle(&self, entries: &[(Vec<u8>, Vec<u32>)]) {
        self.binary(bundle_of(entries));
    }

    /// what it sent is on the socket (ws's send callback)
    pub async fn flush(&self) {
        let (tx, rx) = oneshot::channel();
        let _ = self.tx.send(Cmd::Flush(tx));
        let _ = rx.await;
    }

    pub fn dgrams(&self) -> Vec<Dgram> {
        self.got().dgrams()
    }

    pub fn frames(&self) -> usize {
        self.got().frames.len()
    }

    pub fn is_open(&self) -> bool {
        self.got().closed.is_none() && !self.tasks[0].is_finished()
    }

    /// ws `close()`: a close frame without a code, then its 'close'
    pub async fn close(&self) -> (u16, String) {
        let _ = self.tx.send(Cmd::Send(Message::Close(None)));
        self.closed().await
    }

    /// ws `close(code, reason)`, without waiting
    pub fn close_with(&self, code: u16, reason: &str) {
        let _ = self.tx.send(Cmd::Send(Message::Close(Some(CloseFrame { code: code.into(), reason: reason.into() }))));
    }

    pub async fn closed(&self) -> (u16, String) {
        self.until_ms(5000, |g| g.closed.clone()).await
    }

    /// ws `terminate()`: the connection goes, no close frame
    pub fn terminate(&self) {
        for t in &self.tasks {
            t.abort();
        }
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        self.terminate();
    }
}

/// a client on a plain socket: exact frames, and it reads only when told to
pub async fn raw_ws(port: u16, query: &str) -> TcpStream {
    raw_ws_with(port, query, None).await
}

/// the same with a receive buffer of its own size (SO_RCVBUF)
pub async fn raw_ws_with(port: u16, query: &str, rcvbuf: Option<u32>) -> TcpStream {
    let sock = TcpSocket::new_v4().unwrap();
    if let Some(n) = rcvbuf {
        sock.set_recv_buffer_size(n).unwrap();
    }
    let mut s = sock.connect(([127, 0, 0, 1], port).into()).await.unwrap();
    let mut key = [0u8; 16];
    nfsu2_util::rand::fill(&mut key);
    let key = base64(&key);
    s.write_all(format!("GET /lan{query} HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n\r\n").as_bytes())
        .await
        .unwrap();
    // the answer's head (what follows it stays unread, as a paused socket leaves it)
    let mut head = Vec::new();
    let mut b = [0u8; 1];
    while !head.ends_with(b"\r\n\r\n") {
        assert_eq!(s.read(&mut b).await.unwrap(), 1, "the answer");
        head.push(b[0]);
    }
    assert!(head.starts_with(b"HTTP/1.1 101"), "{}", String::from_utf8_lossy(&head));
    s
}

fn base64(b: &[u8]) -> String {
    const A: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::new();
    for c in b.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 {
            if i <= c.len() {
                s.push(A[(n >> (18 - 6 * i) & 63) as usize] as char);
            } else {
                s.push('=');
            }
        }
    }
    s
}

/// a masked client frame (mask 0: the payload as it is), payload of at most 125 bytes
pub fn frame(op: u8, payload: &[u8]) -> Vec<u8> {
    let mut f = vec![0x80 | op, 0x80 | payload.len() as u8, 0, 0, 0, 0];
    f.extend_from_slice(payload);
    f
}

pub const PING: u8 = 0x9;
pub const PONG: u8 = 0xA;

/// a client on a plain socket that reads only when told to (a slow or stalled receiver), with its hello sent
pub async fn raw_peer(port: u16, name: &str, query: &str) -> TcpStream {
    raw_peer_with(port, name, query, None).await
}

/// the same with a receive buffer of its own size (SO_RCVBUF)
pub async fn raw_peer_with(port: u16, name: &str, query: &str, rcvbuf: Option<u32>) -> TcpStream {
    let mut s = raw_ws_with(port, query, rcvbuf).await;
    s.write_all(&frame(0x1, json!({ "t": "hello", "name": name }).to_string().as_bytes())).await.unwrap();
    s
}

/// a raw peer that reads about `rate` bytes a second (it takes at least one chunk, up to 64 KB, every 250 ms)
pub fn throttle(mut s: TcpStream, rate: usize) -> JoinHandle<()> {
    tokio::spawn(async move {
        let mut buf = vec![0u8; 64 << 10];
        let mut iv = tokio::time::interval(Duration::from_millis(250));
        loop {
            iv.tick().await;
            let mut budget = rate as isize / 4;
            while budget > 0 {
                match tokio::time::timeout(Duration::from_millis(250), s.read(&mut buf)).await {
                    Ok(Ok(0)) | Ok(Err(_)) => return,
                    Ok(Ok(n)) => budget -= n as isize,
                    Err(_) => break,
                }
            }
        }
    })
}

/// a raw socket that reads (and drops) everything, never answering a ping (ws's `autoPong: false`); true once
/// the switch closed it
pub fn drain(mut s: TcpStream) -> JoinHandle<()> {
    tokio::spawn(async move {
        let mut buf = vec![0u8; 16 << 10];
        loop {
            match s.read(&mut buf).await {
                Ok(0) | Err(_) => return,
                Ok(_) => {}
            }
        }
    })
}

/// a host on the switch for the tests (the breakdown's: its address, name and vips(); the browsers it heard leave)
pub struct FakeHost {
    pub vip: u32,
    pub name: String,
    pub listed: bool,
    pub vips: Arc<Mutex<HashSet<u32>>>,
    pub gone: Arc<Mutex<Vec<u32>>>,
}

impl Host for FakeHost {
    fn vip(&self) -> u32 {
        self.vip
    }
    fn name(&self) -> &str {
        &self.name
    }
    fn listed(&self) -> bool {
        self.listed
    }
    fn frame(&self, _from: u32, _data: Bytes, _ctx: nfsu2_util::FrameCtx) {}
    fn peer_gone(&self, vip: u32) {
        self.gone.lock().unwrap().push(vip);
    }
    fn info(&self) -> oneshot::Receiver<Value> {
        let (tx, rx) = oneshot::channel();
        let _ = tx.send(json!({}));
        rx
    }
    fn vips(&self) -> oneshot::Receiver<HashSet<u32>> {
        let (tx, rx) = oneshot::channel();
        let _ = tx.send(self.vips.lock().unwrap().clone());
        rx
    }
    fn stop(&self) {}
}

/// a free-roam hub on the switch for the tests that keeps what it is told of blocks (`Host::blocks`): (vip, names), and
/// (vip, none) for a browser that left (`Host::peer_gone`)
pub struct BlockHub(pub Arc<Mutex<Vec<(u32, Vec<String>)>>>);

impl Host for BlockHub {
    fn vip(&self) -> u32 {
        ip("10.77.0.253")
    }
    fn name(&self) -> &str {
        "hub"
    }
    fn listed(&self) -> bool {
        false
    }
    fn frame(&self, _from: u32, _data: Bytes, _ctx: nfsu2_util::FrameCtx) {}
    fn peer_gone(&self, vip: u32) {
        self.0.lock().unwrap().push((vip, Vec::new()));
    }
    fn info(&self) -> oneshot::Receiver<Value> {
        let (tx, rx) = oneshot::channel();
        let _ = tx.send(json!({}));
        rx
    }
    fn vips(&self) -> oneshot::Receiver<HashSet<u32>> {
        let (tx, rx) = oneshot::channel();
        let _ = tx.send(HashSet::new());
        rx
    }
    fn blocks(&self, vip: u32, names: Vec<String>) {
        self.0.lock().unwrap().push((vip, names));
    }
    fn stop(&self) {}
}

/// node, when it is there (the page tests skip without it)
pub fn node() -> Option<std::path::PathBuf> {
    let out = std::process::Command::new("node").arg("--version").output().ok()?;
    out.status.success().then(|| "node".into())
}

/// web/ (the page's sources and node_modules)
pub fn web_dir() -> std::path::PathBuf {
    std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../..").canonicalize().unwrap()
}

/// a percentile of sorted values
pub fn q(sorted: &[f64], p: f64) -> f64 {
    sorted[((p * sorted.len() as f64) as usize).min(sorted.len() - 1)]
}

pub fn ms_since(t: Instant) -> f64 {
    t.elapsed().as_secs_f64() * 1000.0
}

/// a compact bundle's frames (nfsu2-lan compact.rs), put together again as the page does (web/src/net/proto.ts
/// unpackCompact) for a page holding `own`
pub fn unpack_compact(b: &[u8], own: u32) -> Vec<Vec<u8>> {
    assert_eq!(b[0], 33, "a compact bundle");
    let mut out = Vec::new();
    let mut at = 4;
    while at + 5 <= b.len() {
        let flags = b[at];
        let mut p = at + 5;
        let end = p + u16::from_be_bytes([b[at + 3], b[at + 4]]) as usize;
        if flags == 0x40 {
            out.push(b[p..end].to_vec());
            at = end;
            continue;
        }
        let mut f = vec![flags & 7, 0, 0, 0];
        f.extend_from_slice(&(0x0A4D_0000 | u16::from_be_bytes([b[at + 1], b[at + 2]]) as u32).to_be_bytes());
        f.extend_from_slice(&(if flags & 0x08 != 0 { BROADCAST } else { own }).to_be_bytes());
        if flags & 0x10 != 0 {
            f.extend_from_slice(&b[p..p + 4]);
            p += 4;
        } else {
            f.extend_from_slice(&[0xB8, 0x6A, 0xB8, 0x6A]);
        }
        if flags & 0x20 != 0 {
            let len = end - p + 11;
            let mut d = b"N2FR".to_vec();
            d.extend_from_slice(&[1, b[p]]);
            d.extend_from_slice(&(len as u16).to_le_bytes());
            d.extend_from_slice(&b[p + 1..end]);
            let crc = crc32fast::hash(&d);
            d.extend_from_slice(&crc.to_le_bytes());
            f.extend_from_slice(&d);
        } else {
            f.extend_from_slice(&b[p..end]);
        }
        out.push(f);
        at = end;
    }
    out
}

/// a free-roam datagram as runtime/roam_core.c rc_encode makes it: "N2FR", version 1, its type, the length and the
/// CRC filled in, around a body of n bytes
pub fn roam_datagram(kind: u8, n: usize) -> Vec<u8> {
    let mut d = b"N2FR".to_vec();
    d.extend_from_slice(&[1, kind, 0, 0]);
    d.extend_from_slice(&0x1234_5678u32.to_le_bytes());
    d.extend_from_slice(&[7, 0, 0, 0]);
    d.extend((0..n).map(|i| (i * 37 + 11) as u8));
    let len = (d.len() + 4) as u16;
    d[6..8].copy_from_slice(&len.to_le_bytes());
    let crc = crc32fast::hash(&d);
    d.extend_from_slice(&crc.to_le_bytes());
    d
}
