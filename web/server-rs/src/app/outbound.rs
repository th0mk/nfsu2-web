//! Questions this server asks other servers (Cloudflare's Turnstile: turnstile.rs; Last.fm: lastfm.rs): a form
//! POSTed over HTTPS with the web's root certificates built in (webpki-roots: the image has no certificate store),
//! HTTP/1.1 with the connection closed after the answer.  The caller sets the time limit.

use std::sync::Arc;

use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

/// the most of an answer read
const MAX_ANSWER: usize = 64 << 10;

pub struct Https {
    tls: tokio_rustls::TlsConnector,
}

impl Https {
    pub fn new() -> Https {
        let roots = rustls::RootCertStore::from_iter(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
        let provider = Arc::new(rustls::crypto::ring::default_provider());
        let config = rustls::ClientConfig::builder_with_provider(provider)
            .with_safe_default_protocol_versions()
            .expect("the ring provider's TLS versions")
            .with_root_certificates(roots)
            .with_no_client_auth();
        Https { tls: tokio_rustls::TlsConnector::from(Arc::new(config)) }
    }

    /// a form POSTed to `url` (https://, or http:// for the tests' own servers): the answer's status and body
    pub async fn post_form(&self, url: &str, body: &str) -> Result<(u16, Vec<u8>), String> {
        let (tls, rest) = match url.split_once("://") {
            Some(("https", r)) => (true, r),
            Some(("http", r)) => (false, r),
            _ => return Err(format!("not an http(s) address: {url}")),
        };
        let (hostport, path) = rest.split_once('/').map_or((rest, "/".to_string()), |(h, p)| (h, format!("/{p}")));
        let (host, port) = match hostport.rsplit_once(':') {
            Some((h, p)) if p.bytes().all(|c| c.is_ascii_digit()) => (h, p.parse::<u16>().map_err(|e| e.to_string())?),
            _ => (hostport, if tls { 443 } else { 80 }),
        };
        let req = format!(
            "POST {path} HTTP/1.1\r\nHost: {hostport}\r\nUser-Agent: nfsu2-server\r\nContent-Type: application/x-www-form-urlencoded\r\n\
             Content-Length: {}\r\nConnection: close\r\n\r\n{body}",
            body.len()
        );
        let tcp = tokio::net::TcpStream::connect((host, port)).await.map_err(|e| format!("{host}:{port}: {e}"))?;
        if tls {
            let name = rustls::pki_types::ServerName::try_from(host.to_string()).map_err(|e| format!("{host}: {e}"))?;
            let s = self.tls.connect(name, tcp).await.map_err(|e| format!("{host}: {e}"))?;
            exchange(s, req.as_bytes()).await
        } else {
            exchange(tcp, req.as_bytes()).await
        }
    }
}

/// the request written, the answer read to its end; its status and body
async fn exchange<S: AsyncRead + AsyncWrite + Unpin>(mut s: S, req: &[u8]) -> Result<(u16, Vec<u8>), String> {
    s.write_all(req).await.map_err(|e| e.to_string())?;
    s.flush().await.map_err(|e| e.to_string())?;
    let mut buf = Vec::new();
    let mut chunk = [0u8; 8192];
    loop {
        match s.read(&mut chunk).await {
            Ok(0) => break,
            Ok(n) => {
                buf.extend_from_slice(&chunk[..n]);
                if buf.len() > MAX_ANSWER {
                    return Err("its answer is too long".into());
                }
            }
            // (a TLS server that closes without close_notify: what came is the answer)
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
            Err(e) => return Err(e.to_string()),
        }
    }
    let end = buf.windows(4).position(|w| w == b"\r\n\r\n").ok_or("no answer")?;
    let head = String::from_utf8_lossy(&buf[..end]).to_ascii_lowercase();
    let status = head.split(' ').nth(1).and_then(|c| c.parse::<u16>().ok()).ok_or("no status")?;
    let body = &buf[end + 4..];
    if head.lines().any(|l| l.starts_with("transfer-encoding:") && l.contains("chunked")) {
        return Ok((status, dechunk(body)?));
    }
    Ok((status, body.to_vec()))
}

/// a chunked body's bytes
fn dechunk(mut b: &[u8]) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    loop {
        let eol = b.windows(2).position(|w| w == b"\r\n").ok_or("a broken chunk")?;
        let size = String::from_utf8_lossy(&b[..eol]);
        let n = usize::from_str_radix(size.split(';').next().unwrap_or("").trim(), 16).map_err(|_| "a broken chunk")?;
        b = &b[eol + 2..];
        if n == 0 {
            return Ok(out);
        }
        if b.len() < n {
            return Err("a broken chunk".into());
        }
        out.extend_from_slice(&b[..n]);
        b = b.get(n + 2..).unwrap_or(&[]);
    }
}

/// application/x-www-form-urlencoded
pub fn form(s: &str) -> String {
    let mut o = String::with_capacity(s.len());
    for b in s.bytes() {
        if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.' | b'~') {
            o.push(b as char);
        } else {
            o.push_str(&format!("%{b:02X}"));
        }
    }
    o
}
