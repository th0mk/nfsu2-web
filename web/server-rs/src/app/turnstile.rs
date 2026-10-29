//! Cloudflare Turnstile: the check that a person, not a script, claims a name (/names, names.rs).  The page shows
//! Cloudflare's widget for the site key and sends what it got; the server asks Cloudflare (siteverify) whether that
//! passed, once.  Off unless the server has both keys: NFSU2_TURNSTILE_SITE and NFSU2_TURNSTILE_SECRET (from the
//! Cloudflare dashboard; the secret never on the command line).  With them the page's Content-Security-Policy lets in
//! Cloudflare's script and frame (challenges.cloudflare.com: nfsu2-headers `CSP_TURNSTILE`).
//!
//! The question goes out over HTTPS with the web's root certificates built in (webpki-roots: the image has no
//! certificate store), HTTP/1.1 with the connection closed after the answer, 10 s at most.  NFSU2_TURNSTILE_VERIFY
//! points it elsewhere (the tests' own server, over http://).

use std::sync::Arc;
use std::time::Duration;

use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

pub const VERIFY: &str = "https://challenges.cloudflare.com/turnstile/v0/siteverify";
/// the most of Cloudflare's answer read
const MAX_ANSWER: usize = 64 << 10;
const TIMEOUT: Duration = Duration::from_secs(10);

pub struct Turnstile {
    /// the widget's key (the page's, public)
    pub site: String,
    secret: String,
    verify: String,
    tls: tokio_rustls::TlsConnector,
}

impl Turnstile {
    /// the keys from the environment (both, or none)
    pub fn from_env() -> Option<Turnstile> {
        let get = |k: &str| std::env::var(k).ok().map(|v| v.trim().to_string()).filter(|v| !v.is_empty());
        let (site, secret) = (get("NFSU2_TURNSTILE_SITE")?, get("NFSU2_TURNSTILE_SECRET")?);
        Some(Turnstile::new(site, secret, get("NFSU2_TURNSTILE_VERIFY").unwrap_or_else(|| VERIFY.to_string())))
    }

    pub fn new(site: String, secret: String, verify: String) -> Turnstile {
        let roots = rustls::RootCertStore::from_iter(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
        let provider = Arc::new(rustls::crypto::ring::default_provider());
        let config = rustls::ClientConfig::builder_with_provider(provider)
            .with_safe_default_protocol_versions()
            .expect("the ring provider's TLS versions")
            .with_root_certificates(roots)
            .with_no_client_auth();
        Turnstile { site, secret, verify, tls: tokio_rustls::TlsConnector::from(Arc::new(config)) }
    }

    /// the widget's answer passed Cloudflare's check (Err: Cloudflare could not be asked)
    pub async fn passed(&self, response: &str) -> Result<bool, String> {
        if response.is_empty() || response.len() > 2048 {
            return Ok(false);
        }
        let body = format!("secret={}&response={}", form(&self.secret), form(response));
        let answer = tokio::time::timeout(TIMEOUT, self.post(&body)).await.map_err(|_| "no answer in 10 s".to_string())??;
        let v: serde_json::Value = serde_json::from_slice(&answer).map_err(|e| format!("its answer: {e}"))?;
        Ok(v.get("success").and_then(serde_json::Value::as_bool) == Some(true))
    }

    async fn post(&self, body: &str) -> Result<Vec<u8>, String> {
        let (tls, rest) = match self.verify.split_once("://") {
            Some(("https", r)) => (true, r),
            Some(("http", r)) => (false, r),
            _ => return Err(format!("not an http(s) address: {}", self.verify)),
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

/// the request written, the answer read to its end; its body (Err: not a 200)
async fn exchange<S: AsyncRead + AsyncWrite + Unpin>(mut s: S, req: &[u8]) -> Result<Vec<u8>, String> {
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
    let status = head.split(' ').nth(1).unwrap_or("");
    if status != "200" {
        return Err(format!("answered {status}"));
    }
    let body = &buf[end + 4..];
    if head.lines().any(|l| l.starts_with("transfer-encoding:") && l.contains("chunked")) {
        return dechunk(body);
    }
    Ok(body.to_vec())
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
fn form(s: &str) -> String {
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

#[cfg(test)]
mod tests {
    use super::*;

    /// a server that answers one request with `answer`, and what it was asked
    async fn fake(answer: &'static str) -> (String, tokio::task::JoinHandle<String>) {
        let l = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = format!("http://{}/turnstile/v0/siteverify", l.local_addr().unwrap());
        let h = tokio::spawn(async move {
            let (mut s, _) = l.accept().await.unwrap();
            let mut got = Vec::new();
            let mut b = [0u8; 4096];
            while !String::from_utf8_lossy(&got).contains("response=") {
                let n = s.read(&mut b).await.unwrap();
                got.extend_from_slice(&b[..n]);
            }
            s.write_all(answer.as_bytes()).await.unwrap();
            String::from_utf8_lossy(&got).into_owned()
        });
        (url, h)
    }

    #[tokio::test]
    async fn cloudflare_says_yes_or_no() {
        let (url, h) = fake("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"success\":true}").await;
        let t = Turnstile::new("site".into(), "s3cret&=".into(), url);
        assert_eq!(t.passed("tok en").await, Ok(true));
        let asked = h.await.unwrap();
        assert!(asked.starts_with("POST /turnstile/v0/siteverify HTTP/1.1\r\n"), "{asked}");
        assert!(asked.ends_with("secret=s3cret%26%3D&response=tok%20en"), "{asked}");
        let (url, _) = fake("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n11\r\n{\"success\":false}\r\n0\r\n\r\n").await;
        let t = Turnstile::new("site".into(), "s".into(), url);
        assert_eq!(t.passed("x").await, Ok(false));
        let (url, _) = fake("HTTP/1.1 500 Oops\r\n\r\n").await;
        let t = Turnstile::new("site".into(), "s".into(), url);
        assert!(t.passed("x").await.is_err());
        // nothing to ask about
        assert_eq!(t.passed("").await, Ok(false));
    }
}
