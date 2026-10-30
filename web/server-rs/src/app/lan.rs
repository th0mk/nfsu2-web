//! /lan: the WebSocket upgrade to the virtual LAN switch, every check in its order, and every refusal the same raw
//! answer with this server's headers.  A malformed Sec-WebSocket-Protocol is our 400 too (the Node server's ws library
//! refused it with an answer of its own, which had none of the security headers and named the library).

use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll};
use std::time::Duration;

use tokio::io::{AsyncRead, AsyncWrite, AsyncWriteExt, ReadBuf};

use nfsu2_http::{Hold, Request, Upgraded};
use nfsu2_lan::AttachOpts;
use nfsu2_util::{js, log, vip_str};

use super::{App, Share, url};

/// One half of a /lan connection, holding the client's share of the /lan budget and the connection's own shares (the
/// per-client connection budget, the listener's count: nfsu2_http::Hold): given back once both halves are gone (the
/// connection closed).
pub struct Held<T> {
    inner: T,
    _share: Arc<(Share, Hold)>,
}

impl<T: AsyncRead + Unpin> AsyncRead for Held<T> {
    fn poll_read(self: Pin<&mut Self>, cx: &mut Context<'_>, buf: &mut ReadBuf<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.get_mut().inner).poll_read(cx, buf)
    }
}

impl<T: AsyncWrite + Unpin> AsyncWrite for Held<T> {
    fn poll_write(self: Pin<&mut Self>, cx: &mut Context<'_>, buf: &[u8]) -> Poll<std::io::Result<usize>> {
        Pin::new(&mut self.get_mut().inner).poll_write(cx, buf)
    }
    fn poll_write_vectored(self: Pin<&mut Self>, cx: &mut Context<'_>, bufs: &[std::io::IoSlice<'_>]) -> Poll<std::io::Result<usize>> {
        Pin::new(&mut self.get_mut().inner).poll_write_vectored(cx, bufs)
    }
    fn is_write_vectored(&self) -> bool {
        self.inner.is_write_vectored()
    }
    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.get_mut().inner).poll_flush(cx)
    }
    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
        Pin::new(&mut self.get_mut().inner).poll_shutdown(cx)
    }
}

/// how long the 101 may take to go out: the time any answer has to move (the progress rule's 20 s)
const HANDSHAKE_MOVES: Duration = Duration::from_secs(20);

/// The 101 written, within `within`.  The connection is neither the engine's (its timers stopped at the upgrade) nor
/// the switch's yet (its rules for a client that does not read start once it is attached), so a client that takes
/// nothing (its connection's buffers full of answers it never read) held its /lan share, its connection and the
/// socket here for good.  Node's ws queued the 101 without waiting, and the switch's keepalive cut such a client.
async fn handshake<S: AsyncWrite + Unpin>(stream: &mut S, answer: &[u8], within: Duration) -> bool {
    let write = async {
        stream.write_all(answer).await?;
        stream.flush().await
    };
    matches!(tokio::time::timeout(within, write).await, Ok(Ok(())))
}

/// `/^[+/0-9A-Za-z]{22}==$/`
fn ws_key(k: &str) -> bool {
    let b = k.as_bytes();
    b.len() == 24 && b[..22].iter().all(|&c| c.is_ascii_alphanumeric() || c == b'+' || c == b'/') && &b[22..] == b"=="
}

impl App {
    /// the raw answer for code, then the connection goes (nfsu2_http::raw_refusal)
    async fn refuse(&self, up: Upgraded, code: u16) {
        nfsu2_http::raw_refusal(up.stream, self.raw_response(code)).await;
    }

    /// An upgrade: the target, the Host, the path, the handshake, the Origin, the client's /lan connections and how
    /// fast it opens them, the subprotocols, in that order; then the 101, and the connection to the switch.
    pub async fn on_upgrade(&self, q: Request, up: Upgraded) {
        let Some(u) = url::parse_target(&q.url) else { return self.refuse(up, 400).await };
        if !self.own_host(&q) {
            return self.refuse(up, 421).await;
        }
        // (under the base of the name asked for: /u2/lan)
        if u.pathname.strip_prefix(self.base_of(&q, &u)) != Some("/lan") {
            return self.refuse(up, 404).await;
        }
        // a WebSocket handshake, checked here so that every refusal looks the same
        let key = q.headers.get("sec-websocket-key").unwrap_or("");
        let upgrade = q.headers.get("upgrade").is_some_and(|v| v.to_lowercase() == "websocket");
        if q.method != "GET" || !upgrade || q.headers.get("sec-websocket-version") != Some("13") || !ws_key(key) {
            return self.refuse(up, 400).await;
        }
        // only this server's own pages (a page of another site must not join the LAN through a player's browser);
        // clients without an Origin header (tools) may
        if !self.own_page(&q) {
            return self.refuse(up, 403).await;
        }
        // the console names players by their virtual address and nickname, never by their real address (a VPS's
        // journal keeps it); --access-log is the one opt-in that records addresses
        let (_, key_, site) = self.client(&q);
        let Some(share) = Share::take(&self.lan_conns, &key_, &site) else {
            let line = format!("[lan] refused a connection: {} already open from that address", self.lan_conns.held(&key_));
            log::limited("lan", "refused connections", &key_, &line, 2);
            return self.refuse(up, 429).await;
        };
        if !self.lan_opens.take(&key_, &site) {
            log::limited("lan", "refused connections", &key_, "[lan] refused a connection: that address opens them too fast", 2);
            self.refuse(up, 429).await;
            drop(share);
            return;
        }
        // the subprotocols (a malformed list: 400), last, where the Node server's ws library checked them
        let Ok(protocol) = nfsu2_ws::pick_protocol(q.headers.get("sec-websocket-protocol")) else {
            self.refuse(up, 400).await;
            drop(share);
            return;
        };
        let answer = nfsu2_ws::handshake_response(key, protocol.as_deref());
        let Upgraded { mut stream, pre, hold } = up;
        if !handshake(&mut stream, &answer, HANDSHAKE_MOVES).await {
            return;
        }
        if self.lan_sndbuf > 0 {
            let _ = stream.set_send_buffer(self.lan_sndbuf);
        }
        let (r, w) = stream.split();
        let share = Arc::new((share, hold));
        // the operator's bans (control.rs): by the address's keyed hash now, by its page's ids and its keyphrase at its
        // hello
        let ident = (!key_.is_empty()).then(|| self.bans.addr(&key_));
        let refuse = ident.as_ref().and_then(|a| self.bans.by_addr(a, nfsu2_util::now_ms())).map(|b| {
            log::limited(
                "ban",
                "turned away",
                &key_,
                &format!("[ban] turned away a client of ban #{} \"{}\" (its address)", b.id, b.name),
                2,
            );
            bytes::Bytes::from(b.message())
        });
        let (bans, a, k0) = (self.bans.clone(), ident.clone(), key_.clone());
        let on_hello: nfsu2_lan::OnHello = Box::new(move |h| {
            let seen = nfsu2_bans::Seen { pages: &h.ids, owner: h.owner, addr: a.as_deref() };
            let b = bans.by_hello(&seen, nfsu2_util::now_ms())?;
            let line = format!("[ban] turned away a client of ban #{} \"{}\" (its page or keyphrase)", b.id, b.name);
            log::limited("ban", "turned away", &k0, &line, 2);
            Some(bytes::Bytes::from(b.message()))
        });
        // a reconnecting page says ?resume and sends its old address with the token in its first message (nfsu2-lan);
        // the group and site ids stand for the client's address without naming it (the shared limits)
        let (k1, k2) = (key_.clone(), key_.clone());
        let opts = AttachOpts {
            resume: u.has_param("resume"),
            // a page that can wait in line when every place is taken (nfsu2-lan queue.rs)
            queue: u.has_param("queue"),
            group: Some(self.group_id(&key_)),
            site: (!site.is_empty()).then(|| self.group_id(&format!("site {site}"))),
            on_join: Some(Box::new(move |vip, resumed, n| {
                let line = format!("[lan] {} {} ({n} on the switch)", vip_str(vip), if resumed { "is back" } else { "joined" });
                log::limited("lan", "joins and leaves", &k1, &line, 20);
            })),
            on_leave: Some(Box::new(move |vip, name, n| {
                let line = format!("[lan] {} \"{}\" left ({n} on the switch)", vip_str(vip), js::printable(name, 32));
                log::limited("lan", "joins and leaves", &k2, &line, 20);
            })),
            ident,
            refuse,
            on_hello: Some(on_hello),
            // the keyphrase the page plays with, for the claimed names (names.rs): its owner, never the token
            on_claim: self.names.as_ref().map(|_| Box::new(nfsu2_claims::owner_of_token) as nfsu2_lan::OnClaim),
        };
        self.lan.attach(Held { inner: r, _share: share.clone() }, Held { inner: w, _share: share }, pre, opts);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // A client whose connection takes nothing more (its buffers full of answers it never read) when its 101 is due:
    // let go once the time is up, not waited on for good.  One that reads gets it at once.
    #[tokio::test]
    async fn a_handshake_nobody_takes_is_given_up() {
        let l = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let client = tokio::net::TcpSocket::new_v4().unwrap();
        client.set_recv_buffer_size(4096).unwrap();
        let client = client.connect(l.local_addr().unwrap()).await.unwrap();
        let (mut server, _) = l.accept().await.unwrap();
        // the send buffer filled while the client reads nothing
        let chunk = vec![0u8; 64 << 10];
        loop {
            match server.try_write(&chunk) {
                Ok(_) => {}
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    // (once more after a moment: the kernel moves what it can into the client's window)
                    tokio::time::sleep(Duration::from_millis(50)).await;
                    if server.try_write(&chunk).is_err() {
                        break;
                    }
                }
                Err(e) => panic!("{e}"),
            }
        }
        let t0 = std::time::Instant::now();
        assert!(!handshake(&mut server, b"HTTP/1.1 101 Switching Protocols\r\n\r\n", Duration::from_millis(300)).await);
        assert!(t0.elapsed() < Duration::from_secs(2));
        drop(client);
        // and one that reads
        let client = tokio::net::TcpStream::connect(l.local_addr().unwrap()).await.unwrap();
        let (mut server, _) = l.accept().await.unwrap();
        assert!(handshake(&mut server, b"HTTP/1.1 101 Switching Protocols\r\n\r\n", Duration::from_millis(300)).await);
        drop(client);
    }

    #[test]
    fn websocket_keys() {
        assert!(ws_key("dGhlIHNhbXBsZSBub25jZQ=="));
        assert!(ws_key("+/+/+/+/+/+/+/+/+/+/+/=="));
        assert!(!ws_key("nope"));
        assert!(!ws_key("dGhlIHNhbXBsZSBub25jZQ="));
        assert!(!ws_key("dGhlIHNhbXBsZSBub25jZQ==x"));
        assert!(!ws_key("dGhlIHNhbXBsZSBub25jZ-=="));
    }
}
