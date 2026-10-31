//! A Unix socket's connections (serve_unix: cloudflared's `unix:` origin): answered as a TCP connection's are, each as
//! one from the peer the listener was given (the trusted proxy's 127.0.0.1), and an upgrade (the /lan WebSocket) hands
//! the stream on whole, its send buffer settable as a TCP one's.

use std::net::SocketAddr;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use nfsu2_http::{ClientError, Limits, Request, Response, Service, Upgraded};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{UnixListener, UnixStream};

/// answers "ok" with the peer it was given, and echoes on an upgraded connection
#[derive(Default)]
struct Echo {
    peers: Mutex<Vec<SocketAddr>>,
    sndbuf: Mutex<Option<bool>>,
}

impl Service for Echo {
    fn admit(&self, peer: &SocketAddr) -> Option<Box<dyn Send + Sync>> {
        self.peers.lock().unwrap().push(*peer);
        Some(Box::new(()))
    }
    fn keep_alive(&self, _: &SocketAddr) -> Duration {
        Duration::from_secs(5)
    }
    async fn request<'a>(&'a self, req: Request, res: &'a mut Response) {
        let body = format!("ok {}", req.conn.peer);
        res.head(200, vec![("content-length".into(), body.len().to_string())]);
        let _ = res.write(body.as_bytes()).await;
        let _ = res.end().await;
    }
    async fn expectation<'a>(&'a self, _req: Request, _res: &'a mut Response) {}
    async fn upgrade(&self, _req: Request, mut up: Upgraded) {
        *self.sndbuf.lock().unwrap() = Some(up.stream.set_send_buffer(32 << 10).is_ok());
        let _ = up.stream.write_all(b"HTTP/1.1 101 Switching Protocols\r\n\r\n").await;
        let mut b = [0u8; 64];
        while let Ok(n) = up.stream.read(&mut b).await {
            if n == 0 || up.stream.write_all(&b[..n]).await.is_err() {
                break;
            }
        }
    }
    fn client_error(&self, _: ClientError) -> Option<Vec<u8>> {
        None
    }
}

#[tokio::test]
async fn a_unix_sockets_connections_are_answered_and_upgraded() {
    let dir = std::env::temp_dir().join(format!("nfsu2-unix-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("http.sock");
    let _ = std::fs::remove_file(&path);
    let svc = Arc::new(Echo::default());
    let peer: SocketAddr = ([127, 0, 0, 1], 0).into();
    nfsu2_http::serve_unix(UnixListener::bind(&path).unwrap(), peer, Arc::new(Limits::default()), svc.clone());

    // a request, and a second one on the same connection
    let mut c = UnixStream::connect(&path).await.unwrap();
    for _ in 0..2 {
        c.write_all(b"GET /game.json HTTP/1.1\r\nHost: localhost\r\n\r\n").await.unwrap();
        let mut b = [0u8; 256];
        let n = c.read(&mut b).await.unwrap();
        let s = String::from_utf8_lossy(&b[..n]);
        assert!(s.starts_with("HTTP/1.1 200") && s.ends_with("ok 127.0.0.1"), "{s:?}");
    }

    // an upgrade: the stream itself, both ways
    let mut u = UnixStream::connect(&path).await.unwrap();
    u.write_all(b"GET /lan HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n").await.unwrap();
    let mut b = [0u8; 64];
    let n = u.read(&mut b).await.unwrap();
    assert!(b[..n].starts_with(b"HTTP/1.1 101"), "{:?}", String::from_utf8_lossy(&b[..n]));
    u.write_all(b"ping").await.unwrap();
    let n = tokio::time::timeout(Duration::from_secs(2), u.read(&mut b)).await.unwrap().unwrap();
    assert_eq!(&b[..n], b"ping");

    assert_eq!(*svc.peers.lock().unwrap(), vec![peer, peer], "each connection as from the given peer");
    assert_eq!(*svc.sndbuf.lock().unwrap(), Some(true), "the send buffer is set on a Unix socket too");
    let _ = std::fs::remove_dir_all(&dir);
}
