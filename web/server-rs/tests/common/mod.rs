//! What the integration tests share: a switch (nfsu2-lan) on an ephemeral port of 127.0.0.1 with the Online host
//! (nfsu2-online) and the free-roam hub (nfsu2-roam) attached as src/app/run.rs wires them, fake browsers on its
//! WebSocket (tokio-tungstenite), and what they send: the switch's frames, the virtual host's (nfsu2-online
//! `vhost::lan_frame`), roam_core.c's datagrams, the lobby's messages (as nfsu2-online's `aries` writes and reads
//! them: `encode_frame`, `FrameReader`, `FieldMap`).
#![allow(dead_code)]

use std::collections::HashMap;
use std::sync::atomic::{AtomicU16, AtomicU32, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use bytes::Bytes;
use futures_util::{SinkExt, StreamExt};
use nfsu2_lan::{AttachOpts, LanOptions, LanSwitch};
use nfsu2_online::{OnlineHost, OnlineOptions, VHOST_VIP};
use nfsu2_roam::{ROAM_HUB_VIP, RoamHub, RoamOptions};
use nfsu2_util::Host;
use serde_json::Value;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpSocket, TcpStream};
use tokio::sync::{mpsc, oneshot, watch};
use tokio::task::JoinHandle;
use tokio_tungstenite::tungstenite::Message;

pub const SYN: u8 = 1;
pub const SYNACK: u8 = 2;
pub const RST: u8 = 3;
pub const DATA: u8 = 4;
pub const FIN: u8 = 5;

pub async fn sleep(ms: u64) {
    tokio::time::sleep(Duration::from_millis(ms)).await;
}

/// "10.77.0.5" as a number
pub fn ip(s: &str) -> u32 {
    s.split('.').fold(0u32, |a, b| (a << 8) | b.parse::<u32>().unwrap())
}

pub fn be16(b: &[u8], at: usize) -> u16 {
    u16::from_be_bytes([b[at], b[at + 1]])
}

pub fn be32(b: &[u8], at: usize) -> u32 {
    u32::from_be_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

pub fn le32(b: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]])
}

/// a port for a server process to listen on (it binds it itself): one nobody listens on now, outside the kernel's
/// range for outgoing connections (/proc/sys/net/ipv4/ip_local_port_range), so that no client socket of the tests
/// running beside takes it before the server does, as one of `bind(0)`'s ports can be.
///  - "Nobody listens" is asked by connecting, not by listening on it a moment: on macOS a socket is made without
///    close-on-exec and gets it a moment later, and a server another test thread starts in that moment keeps a
///    listening socket on the port for as long as it runs (seen with lsof: 2 to 5 servers of other tests holding one;
///    the server meant for the port then dies with "port N is in use").
///  - And one no other test process on this machine has picked: another cargo test at the same time (another
///    worktree) walks the same ports, and both could find one free.  A picked port's lock is a file of its own in the
///    temp folder, locked for as long as this process lives (the kernel lets go of it when the process ends).
pub fn free_port() -> u16 {
    static NEXT: AtomicU16 = AtomicU16::new(0);
    static PICKED: Mutex<Vec<std::fs::File>> = Mutex::new(Vec::new());
    let locks = std::env::temp_dir().join("nfsu2-test-ports");
    let _ = std::fs::create_dir_all(&locks);
    let range = std::fs::read_to_string("/proc/sys/net/ipv4/ip_local_port_range").unwrap_or_default();
    let mut r = range.split_whitespace().filter_map(|v| v.parse::<u32>().ok());
    let (lo, hi) = (r.next().unwrap_or(32768), r.next().unwrap_or(60999));
    // 4000 ports below the range, or above it when there is no room below
    let (from, n) = if lo >= 5024 { (lo - 4000, 4000) } else { (hi + 1, (65535 - hi).min(4000)) };
    let start = std::process::id() % n;
    for _ in 0..n {
        let p = (from + (start + NEXT.fetch_add(1, Ordering::Relaxed) as u32) % n) as u16;
        // (no temp folder to lock in: the port alone, as before)
        let lock = std::fs::OpenOptions::new().create(true).truncate(false).write(true).open(locks.join(p.to_string())).ok();
        if lock.as_ref().is_some_and(|f| f.try_lock().is_err()) {
            continue;
        }
        let to = std::net::SocketAddr::from(([127, 0, 0, 1], p));
        let refused = std::net::TcpStream::connect_timeout(&to, Duration::from_secs(1));
        if refused.is_err_and(|e| e.kind() == std::io::ErrorKind::ConnectionRefused) {
            PICKED.lock().unwrap().extend(lock);
            return p;
        }
    }
    panic!("no free port");
}

/// polls f every 10 ms until it says yes (false after `ms`)
pub async fn wait_for(ms: u64, mut f: impl FnMut() -> bool) -> bool {
    let end = tokio::time::Instant::now() + Duration::from_millis(ms);
    loop {
        if f() {
            return true;
        }
        if tokio::time::Instant::now() >= end {
            return false;
        }
        sleep(10).await;
    }
}

// ---- the switch -------------------------------------------------------------------------------------------------

/// The Online host as run.rs makes it, in memory only; the lines it logs, kept
pub fn online_host() -> (Arc<OnlineHost>, Arc<Mutex<Vec<String>>>) {
    let lines = Arc::new(Mutex::new(Vec::new()));
    let l2 = lines.clone();
    let h = OnlineHost::spawn(OnlineOptions {
        state_dir: None,
        log: Arc::new(move |line: &str, _key: Option<&str>| l2.lock().unwrap().push(line.to_string())),
        tap: None,
        claims: None,
    });
    (h, lines)
}

/// The free-roam hub as run.rs makes it (its options the test's)
pub fn roam_hub(o: RoamOptions) -> Arc<RoamHub> {
    RoamHub::spawn(o, Arc::new(|_line: &str| {}))
}

pub struct Switch {
    pub lan: Arc<LanSwitch>,
    pub port: u16,
    accept: JoinHandle<()>,
}

/// a switch with the real hosts, given the switch's way to the browsers as run.rs gives it to them
pub async fn start(o: LanOptions, online: Option<Arc<OnlineHost>>, roam: Option<Arc<RoamHub>>) -> Switch {
    let sw = start_with(o, online.clone().map(|h| h as Arc<dyn Host>), roam.clone().map(|h| h as Arc<dyn Host>)).await;
    if let Some(h) = &online {
        h.attach_switch(sw.lan.switch_send());
    }
    if let Some(h) = &roam {
        h.attach_switch(sw.lan.switch_send());
    }
    sw
}

/// a switch with any hosts (the caller gives them `lan.switch_send()`), on an ephemeral port: /lan?g=<id>&s=<id>
/// stand for the group and site ids the server derives from a client's address; SO_SNDBUF on the switch's sockets
/// as the server sets it (`send_buffer_os`)
pub async fn start_with(o: LanOptions, online: Option<Arc<dyn Host>>, roam: Option<Arc<dyn Host>>) -> Switch {
    let sndbuf = o.send_buffer_os;
    let lan = LanSwitch::new(o, "", online, roam, None, None);
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

/// the upgrade to /lan, answered as the server answers it (src/app/lan.rs), and the connection attached to the switch
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
    let Some(key) = key.filter(|_| path == "/lan") else { return };
    if s.write_all(&nfsu2_ws::handshake_response(&key, None)).await.is_err() {
        return;
    }
    let (mut resume, mut group, mut site) = (false, None, None);
    for kv in query.split('&').filter(|kv| !kv.is_empty()) {
        let (k, v) = kv.split_once('=').unwrap_or((kv, ""));
        match k {
            "resume" => resume = true,
            "g" if group.is_none() => group = Some(v.to_string()),
            "s" if site.is_none() => site = Some(v.to_string()),
            _ => {}
        }
    }
    let pre = Bytes::copy_from_slice(&buf[end..]);
    let (r, w) = s.into_split();
    // the hello's "claim" names the keyphrase's owner, as the server's lan.rs has it
    let on_claim: nfsu2_lan::OnClaim = Box::new(nfsu2_claims::owner_of_token);
    lan.attach(r, w, pre, AttachOpts { resume, group, site, on_claim: Some(on_claim), ..AttachOpts::default() });
}

impl Switch {
    /// the switch stops (its hosts too), its connections end
    pub async fn close(self) {
        self.lan.stop();
        self.lan.terminate_all();
        self.accept.abort();
        sleep(20).await;
    }

    pub async fn info(&self) -> Value {
        self.lan.info().await
    }

    /// the peer on the switch at `vip`, as the tests see it
    pub fn peer(&self, vip: u32) -> Option<nfsu2_lan::PeerView> {
        self.lan.peers_view().into_iter().find(|p| p.vip == vip)
    }

    /// bytes queued to `vip` that the kernel has not taken (Node's `ws.bufferedAmount`)
    pub fn buffered(&self, vip: u32) -> usize {
        self.peer(vip).map_or(0, |p| p.buffered)
    }
}

// ---- frames ---------------------------------------------------------------------------------------------------

/// a frame of the virtual host's TCP (kind 4), UDP (3) or ICMP (5), as nfsu2-online's `vhost::lan_frame` makes one
#[allow(clippy::too_many_arguments)]
pub fn lan_frame(kind: u8, src: u32, dst: u32, sport: u16, dport: u16, op: u8, cid: u32, data: &[u8]) -> Vec<u8> {
    let mut f = vec![0u8; 24 + data.len()];
    f[0] = kind;
    f[4..8].copy_from_slice(&src.to_be_bytes());
    f[8..12].copy_from_slice(&dst.to_be_bytes());
    f[12..14].copy_from_slice(&sport.to_be_bytes());
    f[14..16].copy_from_slice(&dport.to_be_bytes());
    f[16] = op;
    f[17] = 64;
    f[20..24].copy_from_slice(&cid.to_le_bytes());
    f[24..].copy_from_slice(data);
    f
}

/// a datagram with ports to `dst` (kind 1 free roam, 3-5 LAN play), the source left for the switch to write
pub fn udp(dst: u32, sport: u16, dport: u16, kind: u8, data: &[u8]) -> Vec<u8> {
    let mut f = vec![0u8; 16 + data.len()];
    f[0] = kind;
    f[8..12].copy_from_slice(&dst.to_be_bytes());
    f[12..14].copy_from_slice(&sport.to_be_bytes());
    f[14..16].copy_from_slice(&dport.to_be_bytes());
    f[16..].copy_from_slice(data);
    f
}

/// a bundle (the switch's frame kind 32) of frames, each to its own destination (entries of n = 0)
pub fn bundle_of(frames: &[Vec<u8>]) -> Vec<u8> {
    let mut b = vec![32, 0, 0, 0];
    for f in frames {
        b.extend_from_slice(&[0, 0]);
        b.extend_from_slice(&(f.len() as u16).to_be_bytes());
        b.extend_from_slice(f);
    }
    b
}

/// this process's resident memory, in bytes (/proc/self/statm)
pub fn rss() -> u64 {
    let s = std::fs::read_to_string("/proc/self/statm").unwrap_or_default();
    s.split_whitespace().nth(1).and_then(|v| v.parse::<u64>().ok()).unwrap_or(0) * 4096
}

/// CRC-32 (IEEE, zlib's crc32), for roam_core's datagrams
pub fn crc32(b: &[u8]) -> u32 {
    let mut c = !0u32;
    for &x in b {
        c ^= x as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0xEDB8_8320 } else { c >> 1 };
        }
    }
    !c
}

/// a player's datagram of this kind as roam_core.c lays it out: its header, the payload, its CRC
pub fn dgram(kind: u8, sender: u32, payload: &[u8]) -> Vec<u8> {
    let len = 16 + payload.len() + 4;
    let mut b = vec![0u8; len];
    b[..4].copy_from_slice(b"N2FR");
    b[4] = 1;
    b[5] = kind;
    b[6..8].copy_from_slice(&(len as u16).to_le_bytes());
    b[8..12].copy_from_slice(&sender.to_le_bytes());
    b[12..14].copy_from_slice(&7u16.to_le_bytes());
    b[16..16 + payload.len()].copy_from_slice(payload);
    let crc = crc32(&b[..len - 4]);
    b[len - 4..].copy_from_slice(&crc.to_le_bytes());
    b
}

/// a HELLO: room 0, in free roam (status 2), named "p", core version 1
pub fn roam_hello(sender: u32) -> Vec<u8> {
    roam_hello_named(sender, b"p")
}

/// a free-roam HELLO under `name` (core version 1, in free roam, room 0)
pub fn roam_hello_named(sender: u32, name: &[u8]) -> Vec<u8> {
    let mut p = vec![0u8; 13 + name.len() + 1];
    p[0..2].copy_from_slice(&1u16.to_le_bytes());
    p[2] = 1;
    p[3] = 2;
    p[4..6].copy_from_slice(&47210u16.to_le_bytes());
    p[6..8].copy_from_slice(&3u16.to_le_bytes());
    p[12] = name.len() as u8;
    p[13..13 + name.len()].copy_from_slice(name);
    dgram(1, sender, &p)
}

/// a SNAP: the sender's car at `pos`
pub fn roam_snap(sender: u32, pos: [f32; 3]) -> Vec<u8> {
    let mut p = vec![0u8; 6 + 52];
    p[0] = 2;
    p[4] = 1;
    p[10..14].copy_from_slice(&pos[0].to_le_bytes());
    p[14..18].copy_from_slice(&pos[1].to_le_bytes());
    p[18..22].copy_from_slice(&pos[2].to_le_bytes());
    // q.w (a valid quaternion)
    p[6 + 34..6 + 36].copy_from_slice(&32767i16.to_le_bytes());
    dgram(3, sender, &p)
}

/// a datagram to the hub, from port 47210 to 47210
pub fn to_hub(d: &[u8]) -> Vec<u8> {
    udp(ROAM_HUB_VIP, 47210, 47210, 1, d)
}

// ---- the lobby's messages (as nfsu2-online's aries writes them) -----------------------------------------------

/// a field's value as the lobby writes it (nfsu2-online `aries::encode_value`): every byte below 0x20, 0x7F and
/// = " : % ^ as %xx, in quotes when it has a space
pub fn encode_value(v: &str) -> String {
    let mut s = String::new();
    for c in v.bytes() {
        if c < 0x20 || matches!(c, 0x3d | 0x22 | 0x3a | 0x25 | 0x5e | 0x7f) {
            s.push_str(&format!("%{c:02x}"));
        } else {
            s.push(c as char);
        }
    }
    if v.as_bytes().contains(&0x20) { format!("\"{s}\"") } else { s }
}

/// a lobby message with no code (nfsu2-online `aries::encode_frame`): kind, code, length, the fields, a NUL
pub fn encode_frame(kind: &str, fields: &[(&str, String)]) -> Vec<u8> {
    let mut body = String::new();
    for (k, v) in fields {
        body.push_str(&format!("{k}={}\n", encode_value(v)));
    }
    let mut f = vec![0u8; 12];
    f[..kind.len()].copy_from_slice(kind.as_bytes());
    f.extend_from_slice(body.as_bytes());
    f.push(0);
    let n = f.len() as u32;
    f[8..12].copy_from_slice(&n.to_be_bytes());
    f
}

/// one lobby message: its kind, its code (an error tag, "" for success) and its fields (nfsu2-online `aries::FieldMap`)
#[derive(Clone, Debug)]
pub struct Msg {
    pub kind: String,
    pub code: String,
    pub fields: Vec<(String, String)>,
}

impl Msg {
    pub fn get(&self, k: &str) -> Option<&str> {
        self.fields.iter().find(|(x, _)| x == k).map(|(_, v)| v.as_str())
    }
}

fn tag_text(b: &[u8]) -> String {
    b.iter().filter(|&&c| c != 0).map(|&c| c as char).collect()
}

fn unescape(s: &[u8]) -> String {
    let mut out = String::new();
    let mut i = 0;
    while i < s.len() {
        // `/%([0-9a-fA-F]{2})/g`
        if s[i] == b'%' && i + 2 < s.len() && s[i + 1].is_ascii_hexdigit() && s[i + 2].is_ascii_hexdigit() {
            let hex = |c: u8| (c as char).to_digit(16).unwrap_or(0) as u8;
            out.push((hex(s[i + 1]) << 4 | hex(s[i + 2])) as char);
            i += 3;
            continue;
        }
        out.push(s[i] as char);
        i += 1;
    }
    out
}

/// a message body's fields, none of them kept back as secret (nfsu2-online `aries::parse_fields`)
pub fn parse_fields(body: &[u8]) -> Vec<(String, String)> {
    let end = body.iter().position(|&c| c == 0).unwrap_or(body.len());
    let b = &body[..end];
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        while i < b.len() && b[i] <= 0x20 {
            i += 1;
        }
        if i >= b.len() {
            break;
        }
        let mut k = i;
        while k < b.len() && b[k] > 0x20 && b[k] != b'=' {
            k += 1;
        }
        let key = tag_text(&b[i..k]).to_uppercase();
        if k >= b.len() || b[k] != b'=' {
            i = k;
            continue;
        }
        let j = k + 1;
        let raw;
        if j < b.len() && b[j] == b'"' {
            let mut q = j + 1;
            while q < b.len() && b[q] != b'"' {
                q += 1;
            }
            raw = &b[j + 1..q];
            i = q + 1;
        } else {
            let mut q = j;
            while q < b.len() && b[q] > 0x20 {
                q += 1;
            }
            raw = &b[j..q];
            i = q;
        }
        if !out.iter().any(|(x, _): &(String, String)| *x == key) {
            out.push((key, unescape(raw)));
        }
    }
    out
}

/// the messages in a byte stream, each at most 64 KiB (nfsu2-online `aries::FrameReader`)
#[derive(Default)]
pub struct AriesReader {
    buf: Vec<u8>,
    bad: bool,
}

impl AriesReader {
    pub fn push(&mut self, chunk: &[u8]) -> Vec<Msg> {
        if self.bad {
            return Vec::new();
        }
        self.buf.extend_from_slice(chunk);
        let mut out = Vec::new();
        while self.buf.len() >= 12 {
            let n = be32(&self.buf, 8) as usize;
            if !(12..=1 << 16).contains(&n) {
                self.bad = true;
                self.buf.clear();
                break;
            }
            if self.buf.len() < n {
                break;
            }
            let m: Vec<u8> = self.buf.drain(..n).collect();
            out.push(Msg { kind: tag_text(&m[..4]), code: tag_text(&m[4..8]), fields: parse_fields(&m[12..]) });
        }
        out
    }
}

// ---- a fake browser -------------------------------------------------------------------------------------------

/// A TCP connection of the browser's game to the virtual host (runtime/web/net_lan.c as the tests speak it)
#[derive(Default)]
pub struct Tcp {
    pub open: bool,
    pub reset: bool,
    pub fin: bool,
    reader: AriesReader,
    pub msgs: Vec<Msg>,
}

impl Tcp {
    pub fn last(&self, kind: &str) -> Option<&Msg> {
        self.msgs.iter().rev().find(|m| m.kind == kind)
    }
    pub fn count(&self, kind: &str) -> usize {
        self.msgs.iter().filter(|m| m.kind == kind).count()
    }
}

/// What a fake browser got.
#[derive(Default)]
pub struct Got {
    /// binary messages, as they came
    pub frames: Vec<Vec<u8>>,
    /// control messages
    pub ctl: Vec<Value>,
    /// its game's TCP connections to the virtual host, by its own port
    pub tcp: HashMap<u16, Tcp>,
    /// its close: the code and reason of the switch's close frame (1005 without a code, 1006 without a frame)
    pub closed: Option<(u16, String)>,
    /// binary messages counted, not kept (a measurement's clients: their memory is the test process's too)
    pub drop_frames: bool,
    pub frame_count: usize,
    /// the last echo reply's sequence number
    pub echoed: u32,
    echo_seq: u32,
}

impl Got {
    /// the last whole peer list
    pub fn peers(&self) -> Option<Vec<Value>> {
        self.ctl.iter().rev().find(|m| m["t"] == "peers").and_then(|m| m["peers"].as_array().cloned())
    }
    fn frame(&mut self, f: Vec<u8>) {
        self.frame_count += 1;
        // the virtual host's echo replies (ICMP type 0): their sequence numbers, the barrier of `echo()`
        if f.len() >= 32 && f[0] == 5 && f[24] == 0 && be32(&f, 4) == VHOST_VIP {
            self.echoed = u16::from_be_bytes([f[30], f[31]]) as u32;
        }
        if self.drop_frames {
            return;
        }
        if f.len() >= 24
            && f[0] == 4
            && let Some(c) = self.tcp.get_mut(&be16(&f, 14))
        {
            match f[16] {
                SYNACK => c.open = true,
                RST => c.reset = true,
                FIN => c.fin = true,
                DATA => {
                    let m = c.reader.push(&f[24..]);
                    c.msgs.extend(m);
                }
                _ => {}
            }
        }
        self.frames.push(f);
    }
}

enum Cmd {
    Send(Message),
    Flush(oneshot::Sender<()>),
}

static NEXT_PORT: AtomicU16 = AtomicU16::new(50000);
static NEXT_CID: AtomicU32 = AtomicU32::new(1);

/// A connection of the browser's game to the virtual host
#[derive(Clone, Copy, Debug)]
pub struct Conn {
    pub port: u16,
    pub cid: u32,
    pub dport: u16,
}

/// A fake browser: collects control messages and frames, follows its game's TCP connections to the virtual host;
/// its reading can be paused (the kernel's buffers fill, then the switch's queue for it grows: a receiver far
/// behind).
pub struct Browser {
    pub vip: u32,
    /// its welcome's token (what gives its address back to it)
    pub token: String,
    st: Arc<Mutex<Got>>,
    ver: watch::Receiver<u64>,
    tx: mpsc::UnboundedSender<Cmd>,
    paused: watch::Sender<bool>,
    tasks: Vec<JoinHandle<()>>,
}

impl Browser {
    /// ws://127.0.0.1:<port>/lan<query>, with a receive buffer of its own size (SO_RCVBUF) when given; waits for
    /// the welcome
    pub async fn open(port: u16, query: &str, rcvbuf: Option<u32>) -> Browser {
        Browser::open_at(&format!("127.0.0.1:{port}"), port, query, rcvbuf).await
    }

    /// the same with another Host (the binary answers only for its own names)
    pub async fn open_at(host: &str, port: u16, query: &str, rcvbuf: Option<u32>) -> Browser {
        Browser::open_full(host, port, query, rcvbuf, None).await
    }

    /// a page that reconnects (/lan?resume): its hello with its old address and token first, then the welcome
    pub async fn resume(host: &str, port: u16, name: &str, vip: u32, token: &str) -> Browser {
        let hello = serde_json::json!({ "t": "hello", "name": name, "vip": nfsu2_util::vip_str(vip), "token": token });
        Browser::open_full(host, port, "?resume", None, Some(hello.to_string())).await
    }

    /// `first`: a text message sent before the welcome is waited for
    pub async fn open_full(host: &str, port: u16, query: &str, rcvbuf: Option<u32>, first: Option<String>) -> Browser {
        Browser::open_from(host, port, query, rcvbuf, first, None).await
    }

    /// `xff`: the client's address as a proxy on this machine names it (X-Forwarded-For): a client of its own for the
    /// binary's limits per address
    pub async fn open_from(host: &str, port: u16, query: &str, rcvbuf: Option<u32>, first: Option<String>, xff: Option<&str>) -> Browser {
        use tokio_tungstenite::tungstenite::client::IntoClientRequest;
        let sock = TcpSocket::new_v4().unwrap();
        if let Some(n) = rcvbuf {
            sock.set_recv_buffer_size(n).unwrap();
        }
        let s = sock.connect(([127, 0, 0, 1], port).into()).await.expect("connect");
        let _ = s.set_nodelay(true);
        let mut q = format!("ws://{host}/lan{query}").into_client_request().unwrap();
        if let Some(a) = xff {
            q.headers_mut().insert("x-forwarded-for", a.parse().unwrap());
        }
        let (ws, _) = tokio_tungstenite::client_async(q, s).await.expect("the handshake");
        let (mut sink, mut stream) = ws.split();
        let st = Arc::new(Mutex::new(Got::default()));
        let (vtx, ver) = watch::channel(0u64);
        let (tx, mut rx) = mpsc::unbounded_channel::<Cmd>();
        let (paused, mut paused_rx) = watch::channel(false);
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
        let writer_end = writer.abort_handle();
        let s2 = st.clone();
        let reader = tokio::spawn(async move {
            let mut code = None;
            loop {
                // paused: the socket is not read
                while *paused_rx.borrow_and_update() {
                    if paused_rx.changed().await.is_err() {
                        break;
                    }
                }
                let Some(Ok(m)) = stream.next().await else { break };
                {
                    let mut g = s2.lock().unwrap();
                    match m {
                        Message::Binary(b) => g.frame(b.to_vec()),
                        Message::Text(t) => g.ctl.push(serde_json::from_str(t.as_str()).expect("JSON from the switch")),
                        Message::Close(f) => {
                            code = Some(f.map(|f| (u16::from(f.code), f.reason.to_string())).unwrap_or((1005, String::new())))
                        }
                        _ => {}
                    }
                }
                vtx.send_modify(|v| *v += 1);
            }
            writer_end.abort();
            s2.lock().unwrap().closed = Some(code.unwrap_or((1006, String::new())));
            vtx.send_modify(|v| *v += 1);
        });
        let mut b = Browser { vip: 0, token: String::new(), st, ver, tx, paused, tasks: vec![reader, writer] };
        if let Some(t) = first {
            b.text(&t);
        }
        let w = b.until_ms(8000, |g| g.ctl.iter().find(|m| m["t"] == "welcome").cloned()).await;
        b.vip = w["vip"].as_u64().expect("the welcome's vip") as u32;
        b.token = w["token"].as_str().unwrap_or("").to_string();
        b
    }

    /// a connection that may not be admitted: what it got by the time the switch closed it (or 2 s), and how
    pub async fn refused(host: &str, port: u16) -> (Vec<Value>, Option<(u16, String)>) {
        let s = TcpStream::connect(("127.0.0.1", port)).await.expect("connect");
        let (ws, _) = tokio_tungstenite::client_async(format!("ws://{host}/lan"), s).await.expect("the handshake");
        let (_sink, mut stream) = ws.split();
        let mut ctl = Vec::new();
        let mut closed = None;
        let _ = tokio::time::timeout(Duration::from_secs(2), async {
            while let Some(Ok(m)) = stream.next().await {
                match m {
                    Message::Text(t) => ctl.push(serde_json::from_str(t.as_str()).expect("JSON from the switch")),
                    Message::Close(f) => {
                        closed = Some(f.map(|f| (u16::from(f.code), f.reason.to_string())).unwrap_or((1005, String::new())))
                    }
                    _ => {}
                }
            }
        })
        .await;
        (ctl, closed)
    }

    pub fn got(&self) -> MutexGuard<'_, Got> {
        self.st.lock().unwrap()
    }

    /// waits until f says yes (2 s at most)
    pub async fn until<T>(&self, f: impl FnMut(&Got) -> Option<T>) -> T {
        self.until_ms(2000, f).await
    }

    pub async fn until_ms<T>(&self, ms: u64, mut f: impl FnMut(&Got) -> Option<T>) -> T {
        self.try_until(ms, &mut f).await.unwrap_or_else(|| panic!("{}: timed out", nfsu2_util::vip_str(self.vip)))
    }

    /// waits until f says yes; None after `ms` (or once the connection closed without it)
    pub async fn try_until<T>(&self, ms: u64, mut f: impl FnMut(&Got) -> Option<T>) -> Option<T> {
        let deadline = tokio::time::Instant::now() + Duration::from_millis(ms);
        let mut ver = self.ver.clone();
        loop {
            if let Some(v) = f(&self.got()) {
                return Some(v);
            }
            match tokio::time::timeout_at(deadline, ver.changed()).await {
                Ok(Ok(())) => {}
                _ => return f(&self.got()),
            }
        }
    }

    /// a hello as the page sends it (its name, and what it takes)
    pub fn hello(&self, name: &str) {
        self.text(&serde_json::json!({ "t": "hello", "name": name }).to_string());
    }

    pub fn text(&self, s: &str) {
        let _ = self.tx.send(Cmd::Send(Message::text(s)));
    }

    pub fn binary(&self, b: Vec<u8>) {
        let _ = self.tx.send(Cmd::Send(Message::binary(b)));
    }

    /// `binary`, for another task
    pub fn sender(&self) -> impl Fn(Vec<u8>) + Send + Sync + 'static {
        let tx = self.tx.clone();
        move |b| {
            let _ = tx.send(Cmd::Send(Message::binary(b)));
        }
    }

    /// what it sent is on the socket (ws's send callback)
    pub async fn flush(&self) {
        let (tx, rx) = oneshot::channel();
        let _ = self.tx.send(Cmd::Flush(tx));
        let _ = rx.await;
    }

    /// stops reading its socket (true) or reads again (false)
    pub fn pause(&self, on: bool) {
        self.paused.send_replace(on);
    }

    pub fn is_open(&self) -> bool {
        self.got().closed.is_none() && !self.tasks[0].is_finished()
    }

    /// ws `close()`: a close frame without a code, then its 'close'
    pub async fn close(&self) -> (u16, String) {
        let _ = self.tx.send(Cmd::Send(Message::Close(None)));
        self.until_ms(5000, |g| g.closed.clone()).await
    }

    // ---- its game's TCP to the virtual host (10.77.0.1) ----

    /// a connection its game opened over another WebSocket of this browser (a page that reconnected): followed here
    /// from now on
    pub fn adopt(&self, c: Conn) {
        self.got().tcp.insert(c.port, Tcp { open: true, ..Tcp::default() });
    }

    /// a SYN to the virtual host's `dport`, from a port of its own
    pub fn connect_tcp(&self, dport: u16) -> Conn {
        let c = Conn { port: NEXT_PORT.fetch_add(1, Ordering::Relaxed), cid: NEXT_CID.fetch_add(1, Ordering::Relaxed), dport };
        self.got().tcp.insert(c.port, Tcp::default());
        self.tcp_out(c, SYN, &[]);
        c
    }

    pub fn tcp_out(&self, c: Conn, op: u8, data: &[u8]) {
        self.binary(lan_frame(4, self.vip, VHOST_VIP, c.port, c.dport, op, c.cid, data));
    }

    /// one lobby message on connection c
    pub fn send_msg(&self, c: Conn, kind: &str, fields: &[(&str, String)]) {
        self.tcp_out(c, DATA, &encode_frame(kind, fields));
    }

    /// what became of connection c so far
    pub fn tcp<T>(&self, c: Conn, f: impl FnOnce(&Tcp) -> T) -> T {
        f(&self.got().tcp[&c.port])
    }

    /// a connection to the virtual host's port `dport`, accepted
    pub async fn open_tcp(&self, dport: u16) -> Conn {
        self.try_open_tcp(dport).await.expect("the virtual host accepted the connection")
    }

    /// the same, None when it was refused (RST) or not answered in 2 s
    pub async fn try_open_tcp(&self, dport: u16) -> Option<Conn> {
        let c = self.connect_tcp(dport);
        let open = self.try_until(2000, |g| (g.tcp[&c.port].open || g.tcp[&c.port].reset).then_some(g.tcp[&c.port].open)).await;
        open.unwrap_or(false).then_some(c)
    }

    /// a lobby session up to a persona
    pub async fn login(&self, name: &str) -> Conn {
        let c = self.try_login(name).await;
        assert!(c.is_some(), "{name} logged in");
        c.unwrap()
    }

    /// the same, None when the lobby did not take it
    pub async fn try_login(&self, name: &str) -> Option<Conn> {
        let c = self.try_open_tcp(nfsu2_online::PORT_LOBBY).await?;
        self.send_msg(c, "auth", &[("NAME", name.into()), ("PASS", "x".into())]);
        self.send_msg(c, "pers", &[("PERS", name.into())]);
        self.try_until(3000, |g| g.tcp[&c.port].last("pers").map(|_| ())).await.map(|_| c)
    }

    /// a barrier: an ICMP echo to the virtual host, and its reply (the host has taken everything this browser sent
    /// before it, and what it answered is here); false when none came in 2 s
    pub async fn echo(&self) -> bool {
        let seq = {
            let mut g = self.got();
            g.echo_seq = g.echo_seq % 65535 + 1;
            g.echo_seq
        };
        let mut icmp = vec![8, 0, 0, 0, 0, 1];
        icmp.extend_from_slice(&(seq as u16).to_be_bytes());
        self.binary(lan_frame(5, self.vip, VHOST_VIP, 0, 0, 0, 0, &icmp));
        self.try_until(2000, |g| (g.echoed == seq).then_some(())).await.is_some()
    }

    /// the browser's connection ends at once, no close frame (ws `terminate()`)
    pub fn terminate(&self) {
        for t in &self.tasks {
            t.abort();
        }
    }
}

impl Drop for Browser {
    fn drop(&mut self) {
        self.terminate();
    }
}
