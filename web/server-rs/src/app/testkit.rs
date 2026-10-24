//! What the route tests share: a made-up install folder, page folder and module in a temporary folder, the App on an
//! ephemeral port of 127.0.0.1 (no free-roam hub; an Online host with --online on), and a raw client that reads
//! whole answers.

use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

use nfsu2_http::Limits;
use nfsu2_lan::{LanOptions, LanSwitch};

use super::{App, etag};
use crate::options::{RealFs, parse};

pub struct Kit {
    pub dir: tempfile::TempDir,
    pub app: Arc<App>,
    pub addr: SocketAddr,
    server: tokio::task::JoinHandle<()>,
}

impl Drop for Kit {
    fn drop(&mut self) {
        self.server.abort();
    }
}

/// bytes that compress (a few values) or not (xorshift noise)
pub fn data(n: usize, compressible: bool) -> Vec<u8> {
    data_bits(n, if compressible { 2 } else { 8 })
}

/// noise of `bits` bits a byte (7: it compresses by a little, so a run stays about as large as it is)
pub fn data_bits(n: usize, bits: u32) -> Vec<u8> {
    let mut x = 0x2545_f491_4f6c_dd1du64;
    (0..n)
        .map(|_| {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            if bits == 2 { ((x & 3) as u8) * 0x41 } else { (x as u8) & ((1u16 << bits) - 1) as u8 }
        })
        .collect()
}

impl Kit {
    /// the server with these options besides the folders; `files(root)` adds what a test needs to the folder (game/,
    /// repo/web/dist/, repo/out/web/)
    pub async fn start(args: &[&str], files: impl FnOnce(&std::path::Path)) -> Kit {
        Kit::start_with(args, Limits::default(), files).await
    }

    /// `start`, with the engine's limits given (max_connections: the port's count)
    pub async fn start_with(args: &[&str], limits: Limits, files: impl FnOnce(&std::path::Path)) -> Kit {
        Kit::start_full(args, limits, 0, files).await
    }

    /// `start`, with threads that keep the game data's blocks at brotli's best (kept.rs; none in the others)
    pub async fn start_keeping(threads: usize, files: impl FnOnce(&std::path::Path)) -> Kit {
        Kit::start_full(&[], Limits::default(), threads, files).await
    }

    async fn start_full(args: &[&str], limits: Limits, compress_threads: usize, files: impl FnOnce(&std::path::Path)) -> Kit {
        let dir = tempfile::tempdir().expect("a temporary folder");
        let root = std::fs::canonicalize(dir.path()).expect("its real path");
        let p = |r: &str| root.join(r);
        for d in ["game/GLOBAL", "game/TRACKS", "repo/web/dist", "repo/out/web", "state"] {
            std::fs::create_dir_all(p(d)).expect("a folder");
        }
        std::fs::write(p("game/SPEED2.EXE"), b"MZ the game").expect("a file");
        std::fs::write(p("game/GLOBAL/small.bin"), data(1000, true)).expect("a file");
        std::fs::write(p("repo/web/dist/index.html"), b"<!doctype html>").expect("a file");
        files(&root);
        let s = |r: &str| p(r).to_string_lossy().into_owned();
        // (the test's own first: the first of an option counts)
        let mut argv: Vec<String> = args.iter().map(|a| a.to_string()).collect();
        argv.extend(["--data", &s("game"), "--state", &s("state"), "--online", "off", "--roam-hub", "off"].iter().map(|a| a.to_string()));
        let o = parse(&argv, &|_| None, &s("repo"), &s(""), &RealFs).expect("the options");
        let real_state = std::fs::canonicalize(&o.state).expect("the state folder").to_string_lossy().into_owned();
        let key = etag::load_key(&o.state).expect("the ETag key");
        let lan = LanSwitch::new(LanOptions { max_peers: o.max_peers, ..LanOptions::default() }, "", None, None, None, None);
        let vault = (o.vault_gb > 0.0).then(|| {
            let opts = nfsu2_vault::VaultOptions::sized(std::path::PathBuf::from(&o.state).join("vault"), o.vault_gb);
            Arc::new(nfsu2_vault::Vault::open(opts).expect("the vault"))
        });
        let bans = Arc::new(nfsu2_bans::Bans::open(std::path::Path::new(&o.state)).expect("the bans"));
        let online = o.online.then(|| {
            let state_dir = Some(std::path::PathBuf::from(&o.state).join("online"));
            nfsu2_online::OnlineHost::spawn(nfsu2_online::OnlineOptions {
                state_dir,
                log: Arc::new(|_: &str, _: Option<&str>| {}),
                tap: None,
            })
        });
        let app = Arc::new(App::new(o, real_state, key, lan, online, None, vault, bans, 2, 0, compress_threads));
        app.refresh_manifest().await.expect("the manifest");
        let (listener, _) = nfsu2_http::bind("127.0.0.1", 0, None).await.expect("a port");
        let addr = listener.local_addr().expect("its address");
        let server = nfsu2_http::serve(listener, None, Arc::new(limits), app.clone());
        Kit { dir, app, addr, server }
    }

    pub fn path(&self, rel: &str) -> std::path::PathBuf {
        std::fs::canonicalize(self.dir.path()).expect("its real path").join(rel)
    }

    /// a connection with this request written to it
    pub async fn send(&self, path: &str, headers: &[(&str, &str)]) -> TcpStream {
        let mut s = TcpStream::connect(self.addr).await.expect("connect");
        let mut req = format!("GET {path} HTTP/1.1\r\nHost: localhost\r\n");
        for (k, v) in headers {
            req.push_str(&format!("{k}: {v}\r\n"));
        }
        req.push_str("Connection: close\r\n\r\n");
        s.write_all(req.as_bytes()).await.expect("write");
        s
    }

    /// any request, with a body (a Content-Length for it when there is one): the whole answer
    pub async fn request(&self, method: &str, path: &str, headers: &[(&str, &str)], body: &[u8]) -> Answer {
        let mut s = TcpStream::connect(self.addr).await.expect("connect");
        let mut req = format!("{method} {path} HTTP/1.1\r\nHost: localhost\r\n");
        for (k, v) in headers {
            req.push_str(&format!("{k}: {v}\r\n"));
        }
        if !body.is_empty() {
            req.push_str(&format!("Content-Length: {}\r\n", body.len()));
        }
        req.push_str("Connection: close\r\n\r\n");
        let mut all = req.into_bytes();
        all.extend_from_slice(body);
        s.write_all(&all).await.expect("write");
        let mut b = Vec::new();
        tokio::time::timeout(Duration::from_secs(10), s.read_to_end(&mut b)).await.expect("an answer in time").expect("read");
        Answer::parse(&b)
    }

    /// GET path: the whole answer
    pub async fn get(&self, path: &str, headers: &[(&str, &str)]) -> Answer {
        let mut s = self.send(path, headers).await;
        let mut b = Vec::new();
        tokio::time::timeout(Duration::from_secs(10), s.read_to_end(&mut b)).await.expect("an answer in time").expect("read");
        Answer::parse(&b)
    }
}

/// the connection goes with a reset (SO_LINGER 0), as a client that is killed or that drops a request does
pub fn reset(s: TcpStream) {
    let _ = s.set_zero_linger();
    drop(s);
}

/// an answer as the client read it: the status, the headers (names in lower case), the body (de-chunked)
pub struct Answer {
    pub status: u16,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}

impl Answer {
    pub fn parse(b: &[u8]) -> Answer {
        let end = b.windows(4).position(|w| w == b"\r\n\r\n").expect("a whole head");
        let head = String::from_utf8_lossy(&b[..end]).into_owned();
        let mut lines = head.split("\r\n");
        let status = lines.next().and_then(|l| l.split(' ').nth(1)).and_then(|c| c.parse().ok()).expect("a status line");
        let headers: Vec<(String, String)> =
            lines.filter_map(|l| l.split_once(": ")).map(|(k, v)| (k.to_ascii_lowercase(), v.to_string())).collect();
        let mut body = b[end + 4..].to_vec();
        if headers.iter().any(|(k, v)| k == "transfer-encoding" && v == "chunked") {
            let mut out = Vec::new();
            let mut at = 0;
            loop {
                let nl = at + body[at..].windows(2).position(|w| w == b"\r\n").expect("a chunk size");
                let n = usize::from_str_radix(std::str::from_utf8(&body[at..nl]).expect("hex"), 16).expect("hex");
                if n == 0 {
                    break;
                }
                out.extend_from_slice(&body[nl + 2..nl + 2 + n]);
                at = nl + 2 + n + 2;
            }
            body = out;
        }
        Answer { status, headers, body }
    }

    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(k, _)| k == name).map(|(_, v)| v.as_str())
    }
}

/// waits (up to 5 s) for `ok()`
pub async fn eventually(what: &str, mut ok: impl FnMut() -> bool) {
    let t0 = std::time::Instant::now();
    while !ok() {
        assert!(t0.elapsed() < Duration::from_secs(5), "{what}: not in 5 s");
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}
