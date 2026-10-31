//! The server's own HTTP/1.1, on tokio, on every core: what Node's `http.createServer` / `https.createServer` did
//! for the Node server it replaced, with the rules that server set on them ([`Limits`]), and the answers to what it
//! cannot read ([`Service::client_error`], [`raw_refusal`]).  Not hyper: the audits' rules need answers of the
//! server's own to requests a library refuses by itself (with the security headers), the pipelining cut, a
//! keep-alive time per connection, and the connection budget before any TLS.
//!
//! - [`serve`]: the accept loop of one listening socket (plain, or TLS with [`tls::TlsState`])
//! - [`Service`]: what the server does with a connection, a request, an upgrade, a request it cannot read
//! - [`Request`], [`Response`]: one exchange, as Node's `IncomingMessage` / `ServerResponse` were to the Node server
//! - [`Stream`]: a connection, plain or TLS, that an upgrade hands over (the /lan WebSocket), split into halves
//!
//! Inside: `parse` reads requests as llhttp 9 (Node 24's parser) does, `conn` runs a connection as Node's http server
//! did (its timers, the pipelining cut, the refusals), `response` writes answers as Node's `ServerResponse`.  What
//! differs from Node on purpose is said where it is (a request Node would have dropped unanswered when a later one
//! broke the connection is answered first; a TLS handshake has `handshake_timeout` in all, not between its bytes).

mod conn;
mod head;
mod headers;
mod parse;
mod response;
pub mod tls;

/// The unit tests' cross-checks run node itself.
#[cfg(test)]
pub(crate) mod node {
    use std::io::Write;
    use std::process::{Command, Stdio};

    /// `node --input-type=module -e <script>` with `input` on its stdin: its stdout, or None without node (the
    /// caller skips, with a message)
    pub fn run(script: &str, input: &str) -> Option<String> {
        let mut child = Command::new("node")
            .args(["--input-type=module", "-e", script])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .ok()?;
        child.stdin.take()?.write_all(input.as_bytes()).ok()?;
        let out = child.wait_with_output().ok()?;
        assert!(out.status.success(), "node failed");
        String::from_utf8(out.stdout).ok()
    }

    /// a seeded generator (xorshift64*)
    pub struct Rng(pub u64);
    impl Rng {
        pub fn next(&mut self) -> u64 {
            self.0 ^= self.0 >> 12;
            self.0 ^= self.0 << 25;
            self.0 ^= self.0 >> 27;
            self.0.wrapping_mul(0x2545_f491_4f6c_dd1d)
        }
        pub fn below(&mut self, n: usize) -> usize {
            (self.next() % n as u64) as usize
        }
        pub fn pick<'a>(&mut self, list: &[&'a str]) -> &'a str {
            list[self.below(list.len())]
        }
    }
}

use std::future::Future;
use std::io;
use std::net::{IpAddr, SocketAddr};
use std::pin::Pin;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering::Relaxed};
use std::task::{Context, Poll};
use std::time::Duration;

use bytes::Bytes;
use tokio::io::{AsyncRead, AsyncWrite, ReadBuf};
use tokio::net::{TcpListener, TcpStream, UnixListener, UnixStream};
use tokio::sync::mpsc;
use tokio::time::Instant;

/// The limits of one connection and of a listening socket (the ones the Node server set on Node's http server).
#[derive(Clone, Debug)]
pub struct Limits {
    /// request line + headers (Node's maxHeaderSize; above: 431)
    pub max_header_size: usize,
    /// the whole header must arrive within this (from the request's first byte; slow-loris): 408
    pub headers_timeout: Duration,
    /// and the whole request (nothing takes a body but the test PUTs): 408
    pub request_timeout: Duration,
    /// a TLS handshake (Node's default is 120 s)
    pub handshake_timeout: Duration,
    /// a connection with nothing to do for this long goes (Node's server.setTimeout: reads and writes reset it); an
    /// answer the client stopped reading is ended sooner, by the progress rule
    pub socket_idle: Duration,
    /// requests queued on one connection without a finished answer (browsers and proxies send one at a time); one
    /// more and the connection goes
    pub pipelined: usize,
    /// connections at once on one listening socket (Node's maxConnections: more are closed at once)
    pub max_connections: usize,
    /// how often the header and request timeouts are checked (Node's connectionsCheckingInterval)
    pub check_interval: Duration,
    /// a response must move: at least `progress_bytes` in every `progress`... (a 1 MiB block or the 7 MB module takes
    /// seconds; a client that stops reading, or reads a trickle, is let go)
    pub progress_bytes: u64,
    pub progress: Duration,
    /// ...and may take this long at most (the progress rule: conn.rs `progress`)
    pub response_max: Duration,
}

impl Default for Limits {
    fn default() -> Self {
        Limits {
            max_header_size: 16 << 10,
            headers_timeout: Duration::from_secs(15),
            request_timeout: Duration::from_secs(30),
            handshake_timeout: Duration::from_secs(15),
            socket_idle: Duration::from_secs(120),
            pipelined: 4,
            max_connections: 1024,
            check_interval: Duration::from_secs(5),
            progress_bytes: 64 << 10,
            progress: Duration::from_secs(20),
            response_max: Duration::from_secs(15 * 60),
        }
    }
}

/// The HTTP version of a request (Node's `q.httpVersion`).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Version {
    Http10,
    /// HTTP/1.1, and the other versions Node takes (0.9, 2.0), which the server's "HTTP/1.1 needs a Host" counts as
    /// HTTP/1.1: only HTTP/1.0 may come without one ([`Request::http_version`] has the exact version)
    Http11,
}

/// A request's headers as Node's `q.headers` has them: names in lower case, duplicates joined as Node joins them
/// (most with ", "; `cookie` with "; "; the ones Node keeps only once, such as `host`, `content-length`,
/// `content-type`, `user-agent`, `authorization`, `referer`: the first).
#[derive(Clone, Debug, Default)]
pub struct Headers {
    list: Vec<(String, String)>,
}

impl Headers {
    pub fn has(&self, name: &str) -> bool {
        self.get(name).is_some()
    }
    /// every (name, value) after joining, in the order the names first came
    pub fn iter(&self) -> impl Iterator<Item = (&str, &str)> {
        self.list.iter().map(|(k, v)| (k.as_str(), v.as_str()))
    }
}

/// What a request's connection is, for the server's rules.
#[derive(Clone, Debug)]
pub struct ConnInfo {
    /// the TCP peer
    pub peer_addr: SocketAddr,
    /// `socket.remoteAddress` as Node gives it: "127.0.0.1", "::1", "::ffff:1.2.3.4" on a dual-stack listener
    pub peer: String,
    /// TLS (`socket.encrypted`)
    pub encrypted: bool,
}

/// One request (Node's `IncomingMessage`).
pub struct Request {
    pub method: String,
    /// the request target as sent (Node's `q.url`): "/path?query", or an absolute URL
    pub url: String,
    pub version: Version,
    pub headers: Headers,
    pub conn: ConnInfo,
    /// the body, for the few requests that take one (PUT /artifacts/): read with `read_body`
    body: Body,
    major: u8,
    minor: u8,
}

/// The body of a request (Content-Length or chunked).
pub struct Body {
    rx: Option<mpsc::Receiver<conn::BodyMsg>>,
    /// its Content-Length (None: chunked, or no body)
    length: Option<u64>,
}

/// Why a body could not be read.
#[derive(Debug)]
pub enum BodyError {
    /// more than the limit
    TooLarge,
    /// the client went, sent a broken body, or took too long
    Broken,
}

impl Request {
    /// the whole body, at most `limit` bytes
    pub async fn read_body(&mut self, limit: usize) -> Result<Vec<u8>, BodyError> {
        let Some(rx) = self.body.rx.as_mut() else { return Ok(Vec::new()) };
        if self.body.length.is_some_and(|n| n > limit as u64) {
            return Err(BodyError::TooLarge);
        }
        let mut out = Vec::with_capacity(self.body.length.map_or(0, |n| n as usize).min(limit).min(1 << 20));
        loop {
            match rx.recv().await {
                Some(conn::BodyMsg::Data(b)) => {
                    if out.len() + b.len() > limit {
                        return Err(BodyError::TooLarge);
                    }
                    out.extend_from_slice(&b);
                }
                Some(conn::BodyMsg::End) => {
                    self.body.rx = None;
                    return Ok(out);
                }
                None => return Err(BodyError::Broken),
            }
        }
    }
    /// Node's `q.httpVersion`: "1.1", "1.0", "0.9" or "2.0"
    pub fn http_version(&self) -> &'static str {
        match (self.major, self.minor) {
            (1, 0) => "1.0",
            (0, 9) => "0.9",
            (2, 0) => "2.0",
            _ => "1.1",
        }
    }
}

impl nfsu2_headers::Req for Request {
    fn peer(&self) -> &str {
        &self.conn.peer
    }
    fn header(&self, name: &str) -> Option<&str> {
        self.headers.get(name)
    }
    fn encrypted(&self) -> bool {
        self.conn.encrypted
    }
}

/// One answer (Node's `ServerResponse`), written to its connection.  Headers are given in order with lower-case
/// names, as the server gives them to `head`; the engine adds what Node added (`Date`, `Connection`,
/// `Keep-Alive: timeout=<the connection's>` and `Transfer-Encoding: chunked` when there is no Content-Length on
/// HTTP/1.1).  A HEAD request's answer has no body, whatever is written.  Bytes written count towards the
/// response's progress (`Limits::progress_bytes`): an answer the client does not take fast enough is cut, and the
/// connection with it.
pub struct Response {
    io: Option<WriteHalf>,
    out: Vec<u8>,
    flow: Arc<response::Flow>,
    meta: response::Meta,
    status: u16,
    /// what set_header gave (Node's kOutHeaders)
    pre: Vec<(String, String)>,
    /// what head gave
    given: Vec<(String, String)>,
    phase: response::Phase,
    has_body: bool,
    chunked: bool,
    /// the connection goes after this answer (Node's `_last`)
    last: bool,
    /// Node's `shouldKeepAlive`
    keep: bool,
    /// end() without a head: Content-Length 0 (Node's `_contentLength`)
    implicit_len: Option<u64>,
}

/// A request the engine could not read (what it is answered with: [`Service::client_error`]).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum ClientError {
    /// a request Node's parser refuses (an HPE_ error): 400
    Parse,
    /// request line + headers over `max_header_size`: 431
    HeaderOverflow,
    /// the header or the request took too long: 408
    Timeout,
}

/// What the server does with a connection, a request, an upgrade, a request it cannot read.
pub trait Service: Send + Sync + 'static {
    /// A new TCP connection, before any TLS or byte: None closes it at once.  What it returns is dropped when the
    /// connection goes (the per-client connection budget gives its share back).
    fn admit(&self, peer: &SocketAddr) -> Option<Box<dyn Send + Sync>>;
    /// This peer's TLS handshake must show a client certificate that the TLS state's client CA signed, or it fails
    /// (tls.rs; a proxy whose forwarding headers are believed, at addresses others connect from too).  Nobody by
    /// default.
    fn client_certificate(&self, _peer: &SocketAddr) -> bool {
        false
    }
    /// How long an idle keep-alive connection from this peer is kept, and what its answers' `Keep-Alive: timeout=`
    /// says (Cloudflare's own connections: 905 s; others 65 s).
    fn keep_alive(&self, peer: &SocketAddr) -> Duration;
    /// One request.  The engine has counted it against `pipelined` already.
    fn request<'a>(&'a self, req: Request, res: &'a mut Response) -> impl Future<Output = ()> + Send + 'a;
    /// A request with an Expect other than 100-continue on HTTP/1.1 (Node's 'checkExpectation'; for 100-continue the
    /// engine writes "100 Continue" itself and calls `request`).
    fn expectation<'a>(&'a self, req: Request, res: &'a mut Response) -> impl Future<Output = ()> + Send + 'a;
    /// A request that asks for an upgrade (Connection: upgrade and an Upgrade header; Node's 'upgrade' event): the
    /// service takes the connection over, and the 101 or a refusal (`raw_refusal`) is its to write.
    fn upgrade(&self, req: Request, up: Upgraded) -> impl Future<Output = ()> + Send;
    /// A request the engine cannot read: the raw answer to write before the connection goes, or None to close it at
    /// once.
    fn client_error(&self, e: ClientError) -> Option<Vec<u8>>;
}

/// A connection an upgrade hands over, and what was read past the request's head (Node's `head`).
pub struct Upgraded {
    pub stream: Stream,
    pub pre: Bytes,
    /// the connection's shares: keep it for as long as the stream is kept
    pub hold: Hold,
}

/// What a connection holds while it is open: its share of the per-client connection budget (`Service::admit`'s) and
/// its place on the listener's count (`Limits::max_connections`).  An upgrade hands it over with the stream, and
/// whoever takes the stream keeps it until the connection closes: Node counted an upgraded socket (the /lan WebSocket,
/// open for hours) in both until its 'close', and a crowd of WebSockets must not leave the per-client limits and
/// maxConnections free for more connections than the port was sized for.
pub struct Hold(#[allow(dead_code)] Vec<Box<dyn Send + Sync>>);

impl Hold {
    /// holds nothing (a connection made by hand, in a test)
    pub fn none() -> Hold {
        Hold(Vec::new())
    }
}

/// A connection, plain or TLS, or plain on a Unix socket (`serve_unix`).
pub enum Stream {
    Plain(TcpStream),
    Tls(Box<tokio_rustls::server::TlsStream<TcpStream>>),
    Unix(UnixStream),
}

impl Stream {
    /// SO_SNDBUF (the kernel's send buffer for this connection; Linux grows it to megabytes by itself.  On a Unix
    /// socket it holds what waits in the reader's queue: there is no receive buffer of the reader's own)
    pub fn set_send_buffer(&self, bytes: usize) -> io::Result<()> {
        match self {
            Stream::Plain(s) => socket2::SockRef::from(s).set_send_buffer_size(bytes),
            Stream::Tls(s) => socket2::SockRef::from(s.get_ref().0).set_send_buffer_size(bytes),
            Stream::Unix(s) => socket2::SockRef::from(s).set_send_buffer_size(bytes),
        }
    }
    /// a read half and a write half that tasks on different threads can own (plain TCP: lock-free)
    pub fn split(self) -> (ReadHalf, WriteHalf) {
        match self {
            Stream::Plain(s) => {
                let (r, w) = s.into_split();
                (ReadHalf::Plain(r), WriteHalf::Plain(w))
            }
            Stream::Tls(s) => {
                let (r, w) = tokio::io::split(*s);
                (ReadHalf::Tls(r), WriteHalf::Tls(w))
            }
            Stream::Unix(s) => {
                let (r, w) = s.into_split();
                (ReadHalf::Unix(r), WriteHalf::Unix(w))
            }
        }
    }
}

pub enum ReadHalf {
    Plain(tokio::net::tcp::OwnedReadHalf),
    Tls(tokio::io::ReadHalf<tokio_rustls::server::TlsStream<TcpStream>>),
    Unix(tokio::net::unix::OwnedReadHalf),
}
pub enum WriteHalf {
    Plain(tokio::net::tcp::OwnedWriteHalf),
    Tls(tokio::io::WriteHalf<tokio_rustls::server::TlsStream<TcpStream>>),
    Unix(tokio::net::unix::OwnedWriteHalf),
}

impl AsyncRead for ReadHalf {
    fn poll_read(self: Pin<&mut Self>, cx: &mut Context<'_>, buf: &mut ReadBuf<'_>) -> Poll<io::Result<()>> {
        match self.get_mut() {
            ReadHalf::Plain(r) => Pin::new(r).poll_read(cx, buf),
            ReadHalf::Tls(r) => Pin::new(r).poll_read(cx, buf),
            ReadHalf::Unix(r) => Pin::new(r).poll_read(cx, buf),
        }
    }
}
impl AsyncWrite for WriteHalf {
    fn poll_write(self: Pin<&mut Self>, cx: &mut Context<'_>, buf: &[u8]) -> Poll<io::Result<usize>> {
        match self.get_mut() {
            WriteHalf::Plain(w) => Pin::new(w).poll_write(cx, buf),
            WriteHalf::Tls(w) => Pin::new(w).poll_write(cx, buf),
            WriteHalf::Unix(w) => Pin::new(w).poll_write(cx, buf),
        }
    }
    fn poll_write_vectored(self: Pin<&mut Self>, cx: &mut Context<'_>, bufs: &[io::IoSlice<'_>]) -> Poll<io::Result<usize>> {
        match self.get_mut() {
            WriteHalf::Plain(w) => Pin::new(w).poll_write_vectored(cx, bufs),
            WriteHalf::Tls(w) => Pin::new(w).poll_write_vectored(cx, bufs),
            WriteHalf::Unix(w) => Pin::new(w).poll_write_vectored(cx, bufs),
        }
    }
    fn is_write_vectored(&self) -> bool {
        match self {
            WriteHalf::Plain(w) => w.is_write_vectored(),
            WriteHalf::Tls(w) => w.is_write_vectored(),
            WriteHalf::Unix(w) => w.is_write_vectored(),
        }
    }
    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        match self.get_mut() {
            WriteHalf::Plain(w) => Pin::new(w).poll_flush(cx),
            WriteHalf::Tls(w) => Pin::new(w).poll_flush(cx),
            WriteHalf::Unix(w) => Pin::new(w).poll_flush(cx),
        }
    }
    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        match self.get_mut() {
            WriteHalf::Plain(w) => Pin::new(w).poll_shutdown(cx),
            WriteHalf::Tls(w) => Pin::new(w).poll_shutdown(cx),
            WriteHalf::Unix(w) => Pin::new(w).poll_shutdown(cx),
        }
    }
}

impl AsyncRead for Stream {
    fn poll_read(self: Pin<&mut Self>, cx: &mut Context<'_>, buf: &mut ReadBuf<'_>) -> Poll<io::Result<()>> {
        match self.get_mut() {
            Stream::Plain(s) => Pin::new(s).poll_read(cx, buf),
            Stream::Tls(s) => Pin::new(s.as_mut()).poll_read(cx, buf),
            Stream::Unix(s) => Pin::new(s).poll_read(cx, buf),
        }
    }
}
impl AsyncWrite for Stream {
    fn poll_write(self: Pin<&mut Self>, cx: &mut Context<'_>, buf: &[u8]) -> Poll<io::Result<usize>> {
        match self.get_mut() {
            Stream::Plain(s) => Pin::new(s).poll_write(cx, buf),
            Stream::Tls(s) => Pin::new(s.as_mut()).poll_write(cx, buf),
            Stream::Unix(s) => Pin::new(s).poll_write(cx, buf),
        }
    }
    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        match self.get_mut() {
            Stream::Plain(s) => Pin::new(s).poll_flush(cx),
            Stream::Tls(s) => Pin::new(s.as_mut()).poll_flush(cx),
            Stream::Unix(s) => Pin::new(s).poll_flush(cx),
        }
    }
    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        match self.get_mut() {
            Stream::Plain(s) => Pin::new(s).poll_shutdown(cx),
            Stream::Tls(s) => Pin::new(s.as_mut()).poll_shutdown(cx),
            Stream::Unix(s) => Pin::new(s).poll_shutdown(cx),
        }
    }
}

/// Listen on `port` at `host`; a `fallback` host (the server's "::" falls back to "0.0.0.0" on a machine without
/// IPv6, for EAFNOSUPPORT, EADDRNOTAVAIL, EINVAL) is tried when that fails.  Ok: the listener and the host it is on.
/// Err: the message the server dies with after "cannot listen: " ("port N is in use", or the error's as Node wrote
/// it).
pub async fn bind(host: &str, port: u16, fallback: Option<&str>) -> Result<(TcpListener, String), String> {
    let mut host = host.to_string();
    let mut fallback = fallback.map(str::to_string);
    loop {
        match listen_on(&host, port).await {
            Ok(l) => return Ok((l, host)),
            Err(e) => {
                let code = e.raw_os_error().unwrap_or(0);
                if code == libc::EADDRINUSE {
                    return Err(format!("port {port} is in use"));
                }
                let retry = if matches!(code, libc::EAFNOSUPPORT | libc::EADDRNOTAVAIL | libc::EINVAL) { fallback.take() } else { None };
                match retry {
                    Some(f) => host = f,
                    None => return Err(listen_message(&e, &host, port)),
                }
            }
        }
    }
}

async fn listen_on(host: &str, port: u16) -> io::Result<TcpListener> {
    let ip: IpAddr = match host.trim_start_matches('[').trim_end_matches(']').parse() {
        Ok(ip) => ip,
        // a name, as Node's listen() looks it up
        Err(_) => match tokio::net::lookup_host((host, port)).await?.next() {
            Some(a) => a.ip(),
            None => return Err(io::Error::new(io::ErrorKind::NotFound, format!("getaddrinfo ENOTFOUND {host}"))),
        },
    };
    let addr = SocketAddr::new(ip, port);
    let sock = socket2::Socket::new(socket2::Domain::for_address(addr), socket2::Type::STREAM, Some(socket2::Protocol::TCP))?;
    if addr.is_ipv6() {
        // "::" takes IPv4 too (Node's default dual stack)
        sock.set_only_v6(false)?;
    }
    // as libuv: a restarted server may bind while old connections linger in TIME_WAIT
    sock.set_reuse_address(true)?;
    sock.set_nonblocking(true)?;
    sock.bind(&addr.into())?;
    // more than Node's 511: when a Cloudflare Tunnel connection goes, the players on it (a thousand and more) come back
    // within a second, each a new connection from cloudflared; past the backlog the kernel drops the handshake and the
    // client tries again a second or more later.  The kernel takes at most its somaxconn (Linux: 4096 since 5.4)
    sock.listen(4096)?;
    TcpListener::from_std(sock.into())
}

/// Node's message for a listen error: "listen EACCES: permission denied 0.0.0.0:80" (":::80" for "::")
fn listen_message(e: &io::Error, host: &str, port: u16) -> String {
    let (code, text) = match e.raw_os_error() {
        Some(libc::EACCES) => ("EACCES", "permission denied"),
        Some(libc::EPERM) => ("EPERM", "operation not permitted"),
        Some(libc::EADDRNOTAVAIL) => ("EADDRNOTAVAIL", "address not available"),
        Some(libc::EAFNOSUPPORT) => ("EAFNOSUPPORT", "address family not supported"),
        Some(libc::EINVAL) => ("EINVAL", "invalid argument"),
        _ => return e.to_string(),
    };
    format!("listen {code}: {text} {host}:{port}")
}

/// The accept loop of one listening socket, until the process ends: each connection on a task of its own (on every
/// core), TLS first when `tls` is given (within `handshake_timeout`; a handshake that fails or times out closes the
/// connection without a byte of HTTP; a refused one looks as Node's did, `tls::TlsState::handshake`), then requests
/// in turn.
pub fn serve<S: Service>(
    listener: TcpListener,
    tls: Option<Arc<tls::TlsState>>,
    limits: Arc<Limits>,
    service: Arc<S>,
) -> tokio::task::JoinHandle<()> {
    tokio::spawn(accept_loop(Listener::Tcp(listener), tls, limits, service))
}

/// The same for a Unix socket, plain HTTP: a proxy on this machine reaches the server there without TCP (cloudflared's
/// `unix:` origin), and each of its connections counts as one from `peer` (the address the proxy would have on TCP,
/// 127.0.0.1, so it is the trusted proxy it is there).
pub fn serve_unix<S: Service>(listener: UnixListener, peer: SocketAddr, limits: Arc<Limits>, service: Arc<S>) -> tokio::task::JoinHandle<()> {
    tokio::spawn(accept_loop(Listener::Unix(listener, peer), None, limits, service))
}

enum Listener {
    Tcp(TcpListener),
    Unix(UnixListener, SocketAddr),
}

enum Accepted {
    Tcp(TcpStream),
    Unix(UnixStream),
}

impl Listener {
    async fn accept(&self) -> io::Result<(Accepted, SocketAddr)> {
        match self {
            Listener::Tcp(l) => l.accept().await.map(|(s, peer)| (Accepted::Tcp(s), peer)),
            Listener::Unix(l, peer) => l.accept().await.map(|(s, _)| (Accepted::Unix(s), *peer)),
        }
    }
}

/// a connection on the listener's count (Node's server._connections) while it lives
struct Open(Arc<AtomicUsize>);
impl Drop for Open {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Relaxed);
    }
}

async fn accept_loop<S: Service>(listener: Listener, tls: Option<Arc<tls::TlsState>>, limits: Arc<Limits>, service: Arc<S>) {
    // Node checks the header and request times of a server's connections on one interval from its start
    let epoch = Instant::now();
    let open = Arc::new(AtomicUsize::new(0));
    loop {
        let (sock, peer) = match listener.accept().await {
            Ok(c) => c,
            Err(e) => {
                // out of file descriptors (or memory): wait a little, the connections that end give them back
                if matches!(e.raw_os_error(), Some(libc::EMFILE | libc::ENFILE | libc::ENOBUFS | libc::ENOMEM)) {
                    tokio::time::sleep(Duration::from_millis(100)).await;
                }
                continue;
            }
        };
        // Node's maxConnections: more are closed at once, before anything else sees them
        if open.load(Relaxed) >= limits.max_connections {
            drop(sock);
            continue;
        }
        open.fetch_add(1, Relaxed);
        let count = Open(open.clone());
        // Service::admit: the per-client connection budget, before any TLS or byte.  (It runs on this loop: a panic
        // in it costs this connection only, not the loop and with it the port.)
        let Ok(Some(admitted)) = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| service.admit(&peer))) else { continue };
        // Node's http server: noDelay
        if let Accepted::Tcp(tcp) = &sock {
            let _ = tcp.set_nodelay(true);
        }
        let (tls, limits, service) = (tls.clone(), limits.clone(), service.clone());
        // (given back when the connection's task ends, or, for an upgrade, when whoever took the stream lets it go)
        let hold = Hold(vec![admitted, Box::new(count)]);
        tokio::spawn(async move {
            connection(sock, peer, tls, limits, &*service, epoch, hold).await;
        });
    }
}

async fn connection<S: Service>(
    sock: Accepted,
    peer: SocketAddr,
    tls: Option<Arc<tls::TlsState>>,
    limits: Arc<Limits>,
    service: &S,
    epoch: Instant,
    hold: Hold,
) {
    let encrypted = tls.is_some();
    let stream = match (sock, tls) {
        (Accepted::Tcp(tcp), Some(tls)) => {
            // (on the heap for the handshake only: its state, a rustls connection being made, would otherwise be part
            // of the connection's future for as long as the connection lives)
            let accept = Box::pin(tls.handshake(tcp, service.client_certificate(&peer)));
            let done = if limits.handshake_timeout.is_zero() {
                Ok(accept.await)
            } else {
                tokio::time::timeout(limits.handshake_timeout, accept).await
            };
            match done {
                Ok(Some(s)) => Stream::Tls(Box::new(s)),
                // failed or too slow: the connection goes, without a byte of HTTP (an answer queued into a TLS socket
                // whose handshake never finished held the socket for good, and 1024 such sockets locked the port: the
                // phase-2 deploy review's finding 2)
                _ => return,
            }
        }
        (Accepted::Tcp(tcp), None) => Stream::Plain(tcp),
        // (never TLS: serve_unix gives none)
        (Accepted::Unix(s), _) => Stream::Unix(s),
    };
    let info = ConnInfo { peer_addr: peer, peer: remote_address(&peer), encrypted };
    let keep_alive = service.keep_alive(&peer);
    conn::serve(stream, info, limits, keep_alive, epoch, service, hold).await;
}

/// Node's `socket.remoteAddress`: an IPv4 client of a dual-stack listener is "::ffff:a.b.c.d"
fn remote_address(peer: &SocketAddr) -> String {
    match peer.ip() {
        IpAddr::V4(a) => a.to_string(),
        IpAddr::V6(a) => a.to_string(),
    }
}

/// A refusal on a connection taken over by an upgrade: the raw answer, then the connection goes (at once once
/// written, or after 1 s at most).
pub async fn raw_refusal(stream: Stream, answer: Vec<u8>) {
    let (mut r, mut w) = stream.split();
    conn::refuse_io(&mut r, &mut w, &answer).await;
}

#[cfg(test)]
mod tests {
    use super::*;

    /// a service with no state of its own
    struct Nil;
    impl Service for Nil {
        fn admit(&self, _: &SocketAddr) -> Option<Box<dyn Send + Sync>> {
            None
        }
        fn keep_alive(&self, _: &SocketAddr) -> Duration {
            Duration::ZERO
        }
        async fn request<'a>(&'a self, _req: Request, _res: &'a mut Response) {}
        async fn expectation<'a>(&'a self, _req: Request, _res: &'a mut Response) {}
        async fn upgrade(&self, _req: Request, _up: Upgraded) {}
        fn client_error(&self, _: ClientError) -> Option<Vec<u8>> {
            None
        }
    }

    /// What every connection's task holds for as long as it lives, besides the service's own futures: an array kept
    /// across an await, or a handshake's rustls state, would be part of it on every connection, the idle ones' too
    /// (the refusal's 4 KiB read buffer and the handshake made it 6 KiB; idle keep-alive connections come by the
    /// thousand)
    #[tokio::test]
    async fn a_connections_future_holds_no_buffer() {
        let l = TcpListener::bind("127.0.0.1:0").await.expect("bind");
        let c = TcpStream::connect(l.local_addr().expect("addr")).await.expect("connect");
        let (s, peer) = l.accept().await.expect("accept");
        let f = connection(Accepted::Tcp(s), peer, None, Arc::new(Limits::default()), &Nil, Instant::now(), Hold::none());
        let size = std::mem::size_of_val(&f);
        assert!(size < 4096, "{size} bytes");
        drop((f, c));
    }
}
