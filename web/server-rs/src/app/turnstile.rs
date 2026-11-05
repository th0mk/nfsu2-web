//! Cloudflare Turnstile: the check that a person, not a script, claims a name (/names, names.rs).  The page shows
//! Cloudflare's widget for the site key and sends what it got; the server asks Cloudflare (siteverify) whether that
//! passed, once.  Off unless the server has both keys: NFSU2_TURNSTILE_SITE and NFSU2_TURNSTILE_SECRET (from the
//! Cloudflare dashboard; the secret never on the command line).  With them the page's Content-Security-Policy lets in
//! Cloudflare's script and frame (challenges.cloudflare.com: nfsu2-headers `CSP_TURNSTILE`).
//!
//! The question goes out over HTTPS (outbound.rs), 10 s at most.  NFSU2_TURNSTILE_VERIFY
//! points it elsewhere (the tests' own server, over http://).

use std::time::Duration;

use super::outbound::{Https, form};

pub const VERIFY: &str = "https://challenges.cloudflare.com/turnstile/v0/siteverify";
const TIMEOUT: Duration = Duration::from_secs(10);

pub struct Turnstile {
    /// the widget's key (the page's, public)
    pub site: String,
    secret: String,
    verify: String,
    https: Https,
}

impl Turnstile {
    /// the keys from the environment (both, or none)
    pub fn from_env() -> Option<Turnstile> {
        let get = |k: &str| std::env::var(k).ok().map(|v| v.trim().to_string()).filter(|v| !v.is_empty());
        let (site, secret) = (get("NFSU2_TURNSTILE_SITE")?, get("NFSU2_TURNSTILE_SECRET")?);
        Some(Turnstile::new(site, secret, get("NFSU2_TURNSTILE_VERIFY").unwrap_or_else(|| VERIFY.to_string())))
    }

    pub fn new(site: String, secret: String, verify: String) -> Turnstile {
        Turnstile { site, secret, verify, https: Https::new() }
    }

    /// the widget's answer passed Cloudflare's check (Err: Cloudflare could not be asked)
    pub async fn passed(&self, response: &str) -> Result<bool, String> {
        if response.is_empty() || response.len() > 2048 {
            return Ok(false);
        }
        let body = format!("secret={}&response={}", form(&self.secret), form(response));
        let post = self.https.post_form(&self.verify, &body);
        let (status, answer) = tokio::time::timeout(TIMEOUT, post).await.map_err(|_| "no answer in 10 s".to_string())??;
        if status != 200 {
            return Err(format!("answered {status}"));
        }
        let v: serde_json::Value = serde_json::from_slice(&answer).map_err(|e| format!("its answer: {e}"))?;
        Ok(v.get("success").and_then(serde_json::Value::as_bool) == Some(true))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

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
