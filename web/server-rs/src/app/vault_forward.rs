//! /vault on a server whose game keeps no keyphrase saves of its own (Most Wanted's: PLAN D-37, U-26): its pages'
//! requests go on, server to server, to the one vault on the U2 box, over the U2 box's tunnel name that a Cloudflare
//! Access application keeps for this server's service token (U-31 (b), V1).  vault.rs answers them there.
//!
//! What goes on is the page's request as it is: the method, `?earlier`, the token (Authorization), If-Match and
//! If-None-Match, and a PUT's sealed body; this server holds no key and opens no box.  It adds the player's address
//! (`x-vault-client`, as this server reads it from its edge) and the box-to-box credential (`x-vault-box`), which the
//! U2 server takes in place of a browser's Origin check, and only with which it counts the address it is told (one
//! not its own, or any when it has none, it refuses with a 403 and `x-vault-box: refused`, and both boxes log it); and
//! the Access service token (`CF-Access-Client-Id`, `CF-Access-Client-Secret`).  No other header of the browser's goes
//! on (its own `CF-Access-*` never).  What comes back goes to the page only when the vault gave it (every answer it
//! gives a forward it took says so: `x-vault-box: taken`), with the headers that matter (the ETag, `x-vault-*`,
//! Retry-After): its 2xx, 304 and 4xx, its 503 (busy, with Retry-After) and 507 (full).  Anything else is a 503 with the
//! code VAULT-UNREACHABLE, which the page shows as a sync failure while it keeps its local saves: no connection in 5 s,
//! no answer in 20 s, an answer that does not read, the credential refused, and an answer of the way there, which the
//! page would take for the vault's (a wrong address's or a vault-less box's 404 for "no such keyphrase", a 405, 421,
//! an edge's or Access's 401, 403 or 302, the U2 box's own 429 for this box, a tunnel's 5xx).  No retry here: the page
//! asks again.
//!
//! Before anything goes on, this server's own checks: its own pages only (as vault.rs), the four methods, a body only
//! with a PUT and none over the vault's 256 KB.  At most 32 forwards at once; no state written (no folder, no cache of
//! boxes or tokens).  The keys come from the environment of the box, as Turnstile's (never on the command line, in the
//! repository or the image, never in a log line): NFSU2_VAULT_FORWARD (the U2 box's vault, https://<name>/vault),
//! NFSU2_VAULT_BOX_KEY (the credential, the same on both boxes, at least 32 characters) and, behind Access,
//! NFSU2_CF_ACCESS_CLIENT_ID and NFSU2_CF_ACCESS_CLIENT_SECRET.  Each is replaced by restarting with another.
//! A `[vault]` line every 5 minutes counts what was forwarded, refused here (by reason) and failed (by reason).

use std::collections::BTreeMap;
use std::sync::Arc;
use std::time::Duration;

use nfsu2_headers::Cache;
use nfsu2_http::{Request, Response};
use nfsu2_util::log;
use parking_lot::Mutex;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

use super::{App, Fail, Res, finish, present, send, url};
use crate::options::js_to_number;

/// forwards at once
pub const IN_FLIGHT: usize = 32;
/// the U2 box's connection, and its whole answer (assumed; to be set from measurement, TRACKER M11-25)
pub const CONNECT: Duration = Duration::from_secs(5);
pub const TOTAL: Duration = Duration::from_secs(20);
/// the most of the vault's answer read: a version and its head
const MAX_ANSWER: usize = (256 << 10) + (16 << 10);
/// the credential's header, and the player's address's (vault.rs reads them)
pub const BOX_HEADER: &str = "x-vault-box";
pub const CLIENT_HEADER: &str = "x-vault-client";
/// the U2 box's answer to a forward whose credential is not its own (or when it has none), in BOX_HEADER
pub const REFUSED: &str = "refused";
/// in BOX_HEADER, on every answer the U2 box's vault gives a forward it took: what the page may get
pub const TAKEN: &str = "taken";
/// the shortest box-to-box credential taken
pub const MIN_KEY: usize = 32;
/// the vault's headers a page reads, passed back as they came
const PASSED: [&str; 6] = ["etag", "x-vault-saved", "x-vault-earlier", "x-vault-launch-copy", "retry-after", "content-type"];

/// The U2 box's vault, as this server reaches it.
pub struct VaultForward {
    tls: bool,
    host: String,
    port: u16,
    hostport: String,
    path: String,
    key: String,
    access: Option<(String, String)>,
    connector: tokio_rustls::TlsConnector,
    slots: tokio::sync::Semaphore,
    pub connect: Duration,
    pub total: Duration,
    counts: Mutex<Counts>,
}

#[derive(Default)]
struct Counts {
    forwarded: u64,
    refused: BTreeMap<&'static str, u64>,
    failed: BTreeMap<&'static str, u64>,
}

/// the environment's value of k, trimmed; None when unset or empty
fn env(k: &str) -> Option<String> {
    std::env::var(k).ok().map(|v| v.trim().to_string()).filter(|v| !v.is_empty())
}

impl VaultForward {
    /// From the environment: None without NFSU2_VAULT_FORWARD; an error for a forward without its credential, a
    /// credential too short, or one Access key without the other.
    pub fn from_env() -> Result<Option<VaultForward>, String> {
        let Some(url) = env("NFSU2_VAULT_FORWARD") else { return Ok(None) };
        let key = env("NFSU2_VAULT_BOX_KEY").ok_or("NFSU2_VAULT_FORWARD needs NFSU2_VAULT_BOX_KEY: the credential the U2 box takes")?;
        let access = match (env("NFSU2_CF_ACCESS_CLIENT_ID"), env("NFSU2_CF_ACCESS_CLIENT_SECRET")) {
            (Some(i), Some(s)) => Some((i, s)),
            (None, None) => None,
            _ => return Err("NFSU2_CF_ACCESS_CLIENT_ID and NFSU2_CF_ACCESS_CLIENT_SECRET: both or neither".into()),
        };
        VaultForward::new(&url, key, access).map(Some)
    }

    /// to `url` (http:// only for the tests' own servers), with the credential `key` and the Access service token
    pub fn new(url: &str, key: String, access: Option<(String, String)>) -> Result<VaultForward, String> {
        if key.len() < MIN_KEY || !key.bytes().all(|c| c.is_ascii_graphic()) {
            return Err(format!("NFSU2_VAULT_BOX_KEY: at least {MIN_KEY} characters, printable, no spaces"));
        }
        let clean = |v: &str| v.bytes().all(|c| c.is_ascii_graphic());
        if access.as_ref().is_some_and(|(i, s)| !clean(i) || !clean(s)) {
            return Err("NFSU2_CF_ACCESS_CLIENT_ID, NFSU2_CF_ACCESS_CLIENT_SECRET: printable, no spaces".into());
        }
        let (tls, rest) = match url.split_once("://") {
            Some(("https", r)) => (true, r),
            Some(("http", r)) => (false, r),
            _ => return Err(format!("NFSU2_VAULT_FORWARD: not an http(s) address: {url}")),
        };
        let (hostport, path) = rest.split_once('/').map_or((rest, "/".to_string()), |(h, p)| (h, format!("/{p}")));
        let (host, port) = match hostport.rsplit_once(':') {
            Some((h, p)) if !p.is_empty() && p.bytes().all(|c| c.is_ascii_digit()) => (h, p.parse::<u16>().map_err(|e| e.to_string())?),
            _ => (hostport, if tls { 443 } else { 80 }),
        };
        if host.is_empty() || !clean(hostport) || !clean(&path) || path.contains('?') || path.contains('#') {
            return Err(format!("NFSU2_VAULT_FORWARD: no address: {url}"));
        }
        let roots = rustls::RootCertStore::from_iter(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
        let provider = Arc::new(rustls::crypto::ring::default_provider());
        let config = rustls::ClientConfig::builder_with_provider(provider)
            .with_safe_default_protocol_versions()
            .expect("the ring provider's TLS versions")
            .with_root_certificates(roots)
            .with_no_client_auth();
        Ok(VaultForward {
            tls,
            host: host.to_string(),
            port,
            hostport: hostport.to_string(),
            path,
            key,
            access,
            connector: tokio_rustls::TlsConnector::from(Arc::new(config)),
            slots: tokio::sync::Semaphore::new(IN_FLIGHT),
            connect: CONNECT,
            total: TOTAL,
            counts: Mutex::default(),
        })
    }

    /// where it forwards to, for the start-up lines: the address, never a key
    pub fn target(&self) -> String {
        format!(
            "{}://{}{}{}",
            if self.tls { "https" } else { "http" },
            self.hostport,
            self.path,
            if self.access.is_some() { " (with an Access service token)" } else { "" }
        )
    }

    fn refused(&self, why: &'static str) {
        *self.counts.lock().refused.entry(why).or_default() += 1;
    }
    fn failed(&self, why: &'static str) {
        *self.counts.lock().failed.entry(why).or_default() += 1;
    }

    /// the 5-minute line, and the counts start again; None when nothing came
    pub fn line(&self) -> Option<String> {
        let c = std::mem::take(&mut *self.counts.lock());
        let by = |m: &BTreeMap<&str, u64>| m.iter().map(|(k, v)| format!("{v} {k}")).collect::<Vec<_>>().join(", ");
        let (r, f): (u64, u64) = (c.refused.values().sum(), c.failed.values().sum());
        (c.forwarded + r + f > 0).then(|| {
            let mut l = format!("[vault] in the last 5 minutes: {} forwarded", c.forwarded);
            if r > 0 {
                l += &format!(", {r} refused here ({})", by(&c.refused));
            }
            if f > 0 {
                l += &format!(", {f} failed ({})", by(&c.failed));
            }
            l
        })
    }

    /// the request as it goes on
    fn request(&self, q: &Request, earlier: bool, client: &str, body: &[u8]) -> Vec<u8> {
        let mut r = format!(
            "{} {}{} HTTP/1.1\r\nHost: {}\r\nUser-Agent: nfsu2-server\r\n",
            q.method,
            self.path,
            if earlier { "?earlier" } else { "" },
            self.hostport
        );
        for k in ["authorization", "if-match", "if-none-match"] {
            if let Some(v) = q.headers.get(k) {
                r += &format!("{k}: {v}\r\n");
            }
        }
        if q.method == "PUT" {
            r += &format!("content-type: application/octet-stream\r\ncontent-length: {}\r\n", body.len());
        }
        r += &format!("{CLIENT_HEADER}: {client}\r\n{BOX_HEADER}: {}\r\n", self.key);
        if let Some((id, secret)) = &self.access {
            r += &format!("CF-Access-Client-Id: {id}\r\nCF-Access-Client-Secret: {secret}\r\n");
        }
        r += "Connection: close\r\n\r\n";
        let mut b = r.into_bytes();
        b.extend_from_slice(body);
        b
    }

    /// the request sent, the vault's answer read (Err: why it failed, the line's word for it)
    async fn exchange(&self, req: &[u8], head: bool) -> Result<Answer, &'static str> {
        let tcp = match tokio::time::timeout(self.connect, tokio::net::TcpStream::connect((self.host.as_str(), self.port))).await {
            Err(_) => return Err("no connection in time"),
            Ok(Err(_)) => return Err("no connection"),
            Ok(Ok(t)) => t,
        };
        let go = async {
            if self.tls {
                let name = rustls::pki_types::ServerName::try_from(self.host.clone()).map_err(|_| "no connection")?;
                let s = self.connector.connect(name, tcp).await.map_err(|_| "no connection")?;
                talk(s, req, head).await
            } else {
                talk(tcp, req, head).await
            }
        };
        tokio::time::timeout(self.total, go).await.unwrap_or(Err("no answer in time"))
    }
}

/// what the vault answered
struct Answer {
    status: u16,
    headers: Vec<(String, String)>,
    body: Vec<u8>,
}

/// the request written, the answer read to its end (the connection is closed after it)
async fn talk<S: AsyncRead + AsyncWrite + Unpin>(mut s: S, req: &[u8], head: bool) -> Result<Answer, &'static str> {
    s.write_all(req).await.map_err(|_| "the connection broke")?;
    s.flush().await.map_err(|_| "the connection broke")?;
    let mut buf = Vec::new();
    let mut chunk = [0u8; 8192];
    loop {
        match s.read(&mut chunk).await {
            Ok(0) => break,
            Ok(n) => {
                buf.extend_from_slice(&chunk[..n]);
                if buf.len() > MAX_ANSWER {
                    return Err("an answer too long");
                }
            }
            // (a TLS server that closes without close_notify: what came is the answer)
            Err(e) if e.kind() == std::io::ErrorKind::UnexpectedEof => break,
            Err(_) => return Err("the connection broke"),
        }
    }
    let end = buf.windows(4).position(|w| w == b"\r\n\r\n").ok_or("an answer that does not read")?;
    let text = std::str::from_utf8(&buf[..end]).map_err(|_| "an answer that does not read")?;
    let mut lines = text.split("\r\n");
    let status =
        lines.next().and_then(|l| l.split(' ').nth(1)).and_then(|c| c.parse::<u16>().ok()).ok_or("an answer that does not read")?;
    let mut headers = Vec::new();
    for l in lines {
        let (k, v) = l.split_once(':').ok_or("an answer that does not read")?;
        headers.push((k.trim().to_ascii_lowercase(), v.trim().to_string()));
    }
    let get = |k: &str| headers.iter().find(|(n, _)| n == k).map(|(_, v)| v.as_str());
    let raw = &buf[end + 4..];
    let body = if head || status == 304 {
        Vec::new()
    } else if get("transfer-encoding").is_some_and(|t| t.to_ascii_lowercase().contains("chunked")) {
        super::turnstile::dechunk(raw).map_err(|_| "an answer that does not read")?
    } else if let Some(n) = get("content-length") {
        let n: usize = n.parse().map_err(|_| "an answer that does not read")?;
        if raw.len() < n {
            return Err("an answer cut short");
        }
        raw[..n].to_vec()
    } else {
        raw.to_vec()
    };
    Ok(Answer { status, headers, body })
}

/// whose answer it is: the vault's own (it says so in BOX_HEADER), which the page gets as it is; the credential
/// refused; or another's of the way there (Err: the line's word for it): a 404 of a wrong address or of a U2 box with
/// no vault, a 405 of a wrong path, a 421 of a name the U2 box does not serve, an edge's or Access's 401, 403 or 302,
/// the U2 box's 429 for this box (all its players are one client there), a tunnel's 5xx.  Only the vault's goes to the
/// page: any other would read as the vault's (a 404 as "no such keyphrase").
fn whose(a: &Answer) -> Result<(), &'static str> {
    match a.headers.iter().find(|(n, _)| n == BOX_HEADER).map(|(_, v)| v.as_str()) {
        Some(TAKEN) => Ok(()),
        Some(REFUSED) => Err("the credential refused"),
        _ if a.status >= 500 => Err("an error of the way there"),
        _ => Err("an answer not the vault's"),
    }
}

impl App {
    /// /vault forwarded to the U2 box's (this module's doc)
    pub(super) async fn vault_forward(&self, f: &VaultForward, q: &mut Request, s: &mut Response, u: &url::Url) -> Res {
        const CLOSE: &[(&str, &str)] = &[("connection", "close")];
        // only this server's own pages (as vault.rs)
        let site_ok = q.headers.get("sec-fetch-site").is_none_or(|x| x == "same-origin");
        if !self.own_page(q) || !site_ok {
            f.refused("not our page");
            return self.plain(q, s, 403, "Forbidden", &[], Cache::Store, true).await;
        }
        let method = q.method.clone();
        if !matches!(method.as_str(), "GET" | "HEAD" | "PUT" | "DELETE") {
            f.refused("method");
            return self
                .plain(q, s, 405, "Method not allowed", &[("allow", "GET, HEAD, PUT, DELETE"), ("connection", "close")], Cache::Store, true)
                .await;
        }
        let max = nfsu2_vault::VaultOptions::sized(std::path::PathBuf::new(), 1.0).max_body;
        let length = js_to_number(q.headers.get("content-length").unwrap_or("0"));
        if method != "PUT" && (length > 0.0 || present(q, "transfer-encoding")) {
            f.refused("too large");
            return self.plain(q, s, 413, "Too large", CLOSE, Cache::Store, true).await;
        }
        // what goes on must be one header line each: the token as vault.rs takes it, the conditions printable
        let token_ok = q
            .headers
            .get("authorization")
            .and_then(|t| t.strip_prefix("Bearer "))
            .is_some_and(|t| t.len() == 43 && t.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_'));
        if !token_ok {
            f.refused("no token");
            return self.plain(q, s, 401, "A keyphrase's token is needed", &[], Cache::Store, true).await;
        }
        let printable = |k: &str| q.headers.get(k).is_none_or(|v| v.len() <= 200 && v.bytes().all(|c| (0x20..0x7f).contains(&c)));
        if !printable("if-match") || !printable("if-none-match") {
            f.refused("bad request");
            return self.plain(q, s, 400, "Bad request", &[], Cache::Store, true).await;
        }
        let body = if method == "PUT" {
            if length > max as f64 {
                f.refused("too large");
                return self.plain(q, s, 413, "Too large", CLOSE, Cache::Store, true).await;
            }
            match q.read_body(max).await {
                Ok(b) => b,
                Err(nfsu2_http::BodyError::TooLarge) => {
                    f.refused("too large");
                    return self.plain(q, s, 413, "Too large", CLOSE, Cache::Store, true).await;
                }
                Err(nfsu2_http::BodyError::Broken) => {
                    s.destroy();
                    return Err(Fail::Gone);
                }
            }
        } else {
            Vec::new()
        };
        let unreachable = |why: &'static str| {
            f.failed(why);
            why
        };
        let Ok(_slot) = f.slots.try_acquire() else {
            unreachable("too many at once");
            return self.unreachable(q, s).await;
        };
        let (client, _, _) = self.client(q);
        let req = f.request(q, u.has_param("earlier"), &client, &body);
        let a = match f.exchange(&req, method == "HEAD").await {
            Ok(a) => match whose(&a) {
                Ok(()) => a,
                Err(why) => {
                    unreachable(why);
                    // a setting of the boxes' or of the way there: said at once (once a minute at most); a tunnel's 5xx
                    // only in the 5-minute line
                    if why == "the credential refused" {
                        log::limited(
                            "vault",
                            "box key",
                            "",
                            "[vault] the U2 box refused this box's credential: NFSU2_VAULT_BOX_KEY is not the same on both \
                             boxes, or the U2 box has none",
                            1,
                        );
                    } else if a.status < 500 {
                        let l = format!(
                            "[vault] a forward got a {} that is not the U2 box's vault's: NFSU2_VAULT_FORWARD's address, \
                             Access, the U2 box's vault being off, or the U2 box busy",
                            a.status
                        );
                        log::limited("vault", "not the vault's", "", &l, 1);
                    }
                    return self.unreachable(q, s).await;
                }
            },
            Err(why) => {
                unreachable(why);
                return self.unreachable(q, s).await;
            }
        };
        f.counts.lock().forwarded += 1;
        let mut h = self.headers(q, Cache::Store, true);
        for k in PASSED {
            if let Some((_, v)) = a.headers.iter().find(|(n, _)| n == k) {
                h.set(k, v.clone());
            }
        }
        // a HEAD's length is the version's, as the vault said it
        let length = if method == "HEAD" {
            a.headers.iter().find(|(n, _)| n == "content-length").map(|(_, v)| v.clone()).unwrap_or_else(|| "0".into())
        } else {
            a.body.len().to_string()
        };
        if a.status != 304 {
            h.set("content-length", length);
        }
        s.head(a.status, h.0);
        if method != "HEAD" && a.status != 304 {
            send(s, &a.body).await?;
        }
        finish(s).await
    }

    /// the U2 box's vault did not answer: the page shows its sync failure, and keeps its local saves
    async fn unreachable(&self, q: &Request, s: &mut Response) -> Res {
        self.plain(q, s, 503, "The saves' server is not reachable now (VAULT-UNREACHABLE)", &[], Cache::Store, true).await
    }
}

#[cfg(test)]
mod tests {
    use super::super::testkit::{Answer, Kit};
    use super::*;
    use tokio::net::TcpListener;

    const KEY: &str = "box-to-box-credential-of-the-tests-long-enough";
    const T: &str = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    const BEARER: (&str, &str) = ("Authorization", "Bearer AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");

    /// the U2 box: Underground 2's server with keyphrase saves, taking forwards with KEY
    async fn u2() -> Kit {
        Kit::start_set(&[], |_| {}, |a| a.vault_box_key = Some(KEY.into())).await
    }
    /// Most Wanted's server, forwarding to `to` (a vault's address) with KEY and an Access service token
    async fn mw(to: &str, set: impl FnOnce(&mut VaultForward)) -> Kit {
        let to = to.to_string();
        Kit::start_set(
            &["--game", "mw"],
            |root| {
                std::fs::write(root.join("game/speed.exe"), b"MZ").unwrap();
                std::fs::create_dir_all(root.join("repo/web/dist-mw")).unwrap();
                std::fs::write(root.join("repo/web/dist-mw/index.html"), b"<!doctype html>").unwrap();
            },
            move |a| {
                let mut f = VaultForward::new(&to, KEY.into(), Some(("access-id.access".into(), "access-secret".into()))).unwrap();
                set(&mut f);
                a.vault_forward = Some(f);
            },
        )
        .await
    }
    /// what a page sees of an answer: the status, the headers that matter, the body
    fn seen(a: &Answer) -> (u16, Vec<Option<String>>, Vec<u8>) {
        let hs = [
            "etag",
            "x-vault-saved",
            "x-vault-earlier",
            "x-vault-launch-copy",
            "retry-after",
            "content-type",
            "content-length",
            "cache-control",
        ];
        (a.status, hs.iter().map(|h| a.header(h).map(str::to_string)).collect(), a.body.clone())
    }
    /// every file and folder under the state folder
    fn state_files(k: &Kit) -> Vec<String> {
        fn walk(d: &std::path::Path, out: &mut Vec<String>) {
            for e in std::fs::read_dir(d).into_iter().flatten().flatten() {
                out.push(e.path().to_string_lossy().into_owned());
                if e.path().is_dir() {
                    walk(&e.path(), out);
                }
            }
        }
        let mut out = Vec::new();
        walk(&k.path("state"), &mut out);
        out.sort();
        out
    }
    /// headers as the kit takes them
    fn hs<'a>(v: &'a [(&'a str, String)]) -> Vec<(&'a str, &'a str)> {
        v.iter().map(|(k, x)| (*k, x.as_str())).collect()
    }

    // PUT, GET, HEAD, DELETE through Most Wanted's /mw/vault: the same answers as the U2 box's /vault gives its own
    // page for the same requests (one keyphrase through each, in turn), and Most Wanted's server writes nothing
    #[tokio::test]
    async fn the_same_answers_as_the_u2_vault() {
        let a = u2().await;
        let b = u2().await;
        let m = mw(&format!("http://{}/vault", b.addr), |_| {}).await;
        let before = state_files(&m);
        assert!(m.app.vault.is_none() && m.app.names.is_none(), "Most Wanted's server keeps no saves");
        // the same steps at a U2 box's own /vault (a) and through Most Wanted's (to b)
        let steps: Vec<(&str, &str, Vec<(&str, String)>, &[u8])> = vec![
            ("GET", "", vec![], b""),
            ("PUT", "", vec![("If-None-Match", "*".into())], b"box one"),
            ("GET", "", vec![], b""),
            ("HEAD", "", vec![], b""),
            ("PUT", "", vec![("If-None-Match", "*".into())], b"other"),
            ("PUT", "", vec![("If-Match", "\"0000000000000001\"".into())], b"other"),
            ("PUT", "", vec![], b"no condition"),
            ("GET", "?earlier", vec![], b""),
            ("DELETE", "", vec![], b""),
            ("DELETE", "", vec![], b""),
        ];
        for (i, (method, query, extra, body)) in steps.iter().enumerate() {
            let mut h: Vec<(&str, String)> = vec![(BEARER.0, BEARER.1.to_string())];
            h.extend(extra.iter().cloned());
            let x = a.request(method, &format!("/vault{query}"), &hs(&h), body).await;
            let y = m.request(method, &format!("/mw/vault{query}"), &hs(&h), body).await;
            let (mut sx, mut sy) = (seen(&x), seen(&y));
            // the versions' tags and times are each vault's own: the same shape
            for s in [&mut sx, &mut sy] {
                if let Some(e) = s.1[0].take() {
                    assert!(e.len() == 18 && e.starts_with('"'), "step {i}: {e}");
                }
                s.1[1] = s.1[1].take().map(|v| v.len().to_string());
            }
            assert_eq!(sx, sy, "step {i}: {method}{query}");
        }
        // a version saved through Most Wanted's server, read at the U2 box's own /vault: the same box
        let p = m.request("PUT", "/mw/vault", &[BEARER, ("If-None-Match", "*")], b"sealed bytes").await;
        assert_eq!(p.status, 201);
        let g = b.request("GET", "/vault", &[BEARER], b"").await;
        assert_eq!((g.status, g.body.as_slice(), g.header("etag")), (200, &b"sealed bytes"[..], p.header("etag")));
        // a page that has the current version: 304 through it too
        let n = m.request("GET", "/mw/vault", &[BEARER, ("If-None-Match", p.header("etag").unwrap())], b"").await;
        assert_eq!((n.status, n.body.len()), (304, 0));
        assert_eq!(state_files(&m), before, "Most Wanted's state folder: no new file");
        assert!(m.app.vault_forward.as_ref().unwrap().line().unwrap().contains("forwarded"));
        assert_eq!(T.len(), 43);
    }

    // The player's address: with the credential the U2 box counts the address Most Wanted's server names (31 misses
    // from one player: the miss bucket's answer; a second player unaffected); a browser's own x-vault-client is
    // nothing without the credential; a forward without it or with another is a browser's request
    #[tokio::test]
    async fn the_players_address_counts_with_the_credential_only() {
        let b = u2().await;
        let m = mw(&format!("http://{}/vault", b.addr), |_| {}).await;
        let t = |i: usize| format!("Bearer {i:0>43}");
        for i in 0..30 {
            let r = m.request("GET", "/mw/vault", &[("Authorization", &t(i)), ("X-Forwarded-For", "198.51.100.7")], b"").await;
            assert_eq!(r.status, 404, "miss {i}");
        }
        let r = m.request("GET", "/mw/vault", &[("Authorization", &t(99)), ("X-Forwarded-For", "198.51.100.7")], b"").await;
        assert_eq!(r.status, 429, "the 31st miss of one player");
        let r = m.request("GET", "/mw/vault", &[("Authorization", &t(99)), ("X-Forwarded-For", "198.51.100.8")], b"").await;
        assert_eq!(r.status, 404, "another player: its own budget");
        // straight at the U2 box: a forged address header is a browser's request (the loopback client's own budget,
        // untouched by the misses above); a wrong credential is refused, and says so
        let tt = t(98);
        assert_eq!(b.request("GET", "/vault", &[("Authorization", &tt), ("x-vault-client", "198.51.100.7")], b"").await.status, 404);
        let r = b
            .request("GET", "/vault", &[("Authorization", &tt), ("x-vault-client", "198.51.100.7"), ("x-vault-box", "not the key")], b"")
            .await;
        assert_eq!((r.status, r.header("x-vault-box")), (403, Some(REFUSED)));
        // a request of another site's page, with the credential's header but not the credential: refused
        let r = b.request("GET", "/vault", &[BEARER, ("Origin", "https://elsewhere.example"), ("x-vault-box", "not the key")], b"").await;
        assert_eq!(r.status, 403);
        // with the credential, the page's check is the forwarder's: an address it names is needed (the vault's answer,
        // which says so); a page's own request has no such mark
        let r = b.request("GET", "/vault", &[BEARER, ("x-vault-box", KEY)], b"").await;
        assert_eq!((r.status, r.header("x-vault-box")), (400, Some(TAKEN)));
        let r = b.request("GET", "/vault", &[BEARER], b"").await;
        assert_eq!((r.status, r.header("x-vault-box")), (404, None));
    }

    // The boxes' credentials not the same, or none on the U2 box: every forward is refused there (no player shares
    // the Most Wanted box's one address budget, as a browser's request would), the page gets the unreachable 503, and
    // Most Wanted's line counts it
    #[tokio::test]
    async fn a_credential_the_u2_box_does_not_take_is_refused() {
        let other = "another-box-to-box-credential-also-long-enough";
        for b in [Kit::start_set(&[], |_| {}, |a| a.vault_box_key = Some(other.into())).await, Kit::start(&[], |_| {}).await] {
            let m = mw(&format!("http://{}/vault", b.addr), |_| {}).await;
            for (i, ip) in ["198.51.100.7"; 31].iter().chain(&["198.51.100.8", "198.51.100.9"]).enumerate() {
                let t = format!("Bearer {i:0>43}");
                let r = m.request("GET", "/mw/vault", &[("Authorization", &t), ("X-Forwarded-For", ip)], b"").await;
                assert_eq!(r.status, 503, "{i} {ip}");
                assert!(String::from_utf8_lossy(&r.body).contains("VAULT-UNREACHABLE"));
                assert_eq!(r.header("x-vault-box"), None, "the U2 box's word stays between the boxes");
            }
            assert_eq!(m.request("PUT", "/mw/vault", &[BEARER, ("If-None-Match", "*")], b"box").await.status, 503);
            assert_eq!(b.app.vault.as_ref().unwrap().store.usage().0, 0, "nothing saved");
            let line = m.app.vault_forward.as_ref().unwrap().line().unwrap();
            assert!(line.contains("0 forwarded") && line.contains("34 failed (34 the credential refused)"), "{line}");
        }
    }

    // Refused here, before anything goes on: another site's page, another method, a body past the vault's 256 KB, no
    // token.  The U2 box's vault never hears of them.
    #[tokio::test]
    async fn refused_before_forwarding() {
        let b = u2().await;
        let m = mw(&format!("http://{}/vault", b.addr), |_| {}).await;
        assert_eq!(m.request("GET", "/mw/vault", &[BEARER, ("Origin", "https://elsewhere.example")], b"").await.status, 403);
        assert_eq!(m.request("GET", "/mw/vault", &[BEARER, ("Sec-Fetch-Site", "cross-site")], b"").await.status, 403);
        assert_eq!(m.request("POST", "/mw/vault", &[BEARER], b"").await.status, 405);
        assert_eq!(m.request("GET", "/mw/vault", &[], b"").await.status, 401);
        // a body past 256 KB, said before it is sent (the answer comes before it: the client sends none of it)
        let mut s = tokio::net::TcpStream::connect(m.addr).await.unwrap();
        let head = format!(
            "PUT /mw/vault HTTP/1.1\r\nHost: localhost\r\n{}: {}\r\nIf-None-Match: *\r\nContent-Length: {}\r\n\r\n",
            BEARER.0,
            BEARER.1,
            (256 << 10) + 1
        );
        s.write_all(head.as_bytes()).await.unwrap();
        let mut got = vec![0u8; 512];
        let n = s.read(&mut got).await.unwrap();
        assert!(got[..n].starts_with(b"HTTP/1.1 413 "), "{}", String::from_utf8_lossy(&got[..n]));
        assert_eq!(m.request("GET", "/mw/vault", &[BEARER], b"body").await.status, 413);
        assert_eq!(b.app.vault.as_ref().unwrap().store.usage().0, 0, "nothing reached the vault");
        let line = m.app.vault_forward.as_ref().unwrap().line().unwrap();
        assert!(line.contains("0 forwarded") && line.contains("6 refused here"), "{line}");
        // the largest version goes
        let most = vec![7u8; 256 << 10];
        assert_eq!(m.request("PUT", "/mw/vault", &[BEARER, ("If-None-Match", "*")], &most).await.status, 201);
    }

    // The U2 box away: no connection (its server stopped) is a 503 within the connect timeout, a server that takes
    // the connection and never answers a 503 at the total timeout, and an error page of the way there a 503 too; each
    // with the code the page shows
    #[tokio::test]
    async fn the_u2_box_away_is_a_503() {
        // a port nothing listens on (1, tcpmux: no server of a test's takes it, as a freed port of its own might be)
        let m = mw("http://127.0.0.1:1/vault", |f| f.connect = Duration::from_millis(300)).await;
        let t0 = std::time::Instant::now();
        let r = m.request("GET", "/mw/vault", &[BEARER], b"").await;
        assert_eq!(r.status, 503);
        assert!(String::from_utf8_lossy(&r.body).contains("VAULT-UNREACHABLE"));
        assert_eq!(r.header("retry-after"), None, "not the vault's 'busy for now'");
        assert!(t0.elapsed() < Duration::from_secs(2));
        // a server that never answers
        let stall = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = stall.local_addr().unwrap();
        let held = tokio::spawn(async move {
            let mut kept = Vec::new();
            while let Ok((s, _)) = stall.accept().await {
                kept.push(s);
            }
        });
        let m = mw(&format!("http://{addr}/vault"), |f| f.total = Duration::from_millis(700)).await;
        let t0 = std::time::Instant::now();
        let r = m.request("PUT", "/mw/vault", &[BEARER, ("If-None-Match", "*")], b"box").await;
        let took = t0.elapsed();
        assert_eq!(r.status, 503);
        assert!(took >= Duration::from_millis(650) && took < Duration::from_secs(3), "{took:?}");
        held.abort();
        // the tunnel's or the edge's error page
        let edge = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = edge.local_addr().unwrap();
        tokio::spawn(async move {
            while let Ok((mut s, _)) = edge.accept().await {
                let mut b = [0u8; 4096];
                let _ = s.read(&mut b).await;
                let _ = s.write_all(b"HTTP/1.1 502 Bad Gateway\r\ncontent-type: text/html\r\ncontent-length: 5\r\n\r\noops!").await;
            }
        });
        let m = mw(&format!("http://{addr}/vault"), |_| {}).await;
        let r = m.request("GET", "/mw/vault", &[BEARER], b"").await;
        assert_eq!(r.status, 503);
        let line = m.app.vault_forward.as_ref().unwrap().line().unwrap();
        assert!(line.contains("1 failed (1 an error of the way there)"), "{line}");
    }

    // Answers of the way there, not the vault's, are the unreachable 503 too, never the page's "no such keyphrase" or
    // another of the vault's: a forward to a wrong path (the U2 box's 404 and 405), to a U2 box without keyphrase saves,
    // and an edge's, Access's or a busy U2 box's 401, 403, 302, 429 and 404
    #[tokio::test]
    async fn answers_not_the_vaults_are_a_503() {
        let b = u2().await;
        let m = mw(&format!("http://{}/vaults", b.addr), |_| {}).await;
        assert_eq!(m.request("GET", "/mw/vault", &[BEARER], b"").await.status, 503);
        assert_eq!(m.request("PUT", "/mw/vault", &[BEARER, ("If-None-Match", "*")], b"box").await.status, 503);
        let line = m.app.vault_forward.as_ref().unwrap().line().unwrap();
        assert!(line.contains("0 forwarded") && line.contains("2 failed (2 an answer not the vault's)"), "{line}");
        let none = Kit::start_set(&["--vault-gb", "0"], |_| {}, |a| a.vault_box_key = Some(KEY.into())).await;
        let m = mw(&format!("http://{}/vault", none.addr), |_| {}).await;
        assert_eq!(m.request("GET", "/mw/vault", &[BEARER], b"").await.status, 503);
        assert_eq!(m.request("DELETE", "/mw/vault", &[BEARER], b"").await.status, 503);
        // the edge's answers
        let edge = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = edge.local_addr().unwrap();
        let answers: [&[u8]; 5] = [
            b"HTTP/1.1 401 Unauthorized\r\ncontent-length: 0\r\n\r\n",
            b"HTTP/1.1 403 Forbidden\r\ncontent-type: text/html\r\ncontent-length: 4\r\n\r\nnope",
            b"HTTP/1.1 302 Found\r\nlocation: https://x.example/cdn-cgi/access/login\r\ncontent-length: 0\r\n\r\n",
            b"HTTP/1.1 429 Too Many Requests\r\nretry-after: 1\r\ncontent-length: 0\r\n\r\n",
            b"HTTP/1.1 404 Not Found\r\ncontent-length: 10\r\n\r\nNot found\n",
        ];
        tokio::spawn(async move {
            for a in answers.iter().cycle() {
                let Ok((mut s, _)) = edge.accept().await else { break };
                let mut b = [0u8; 4096];
                let _ = s.read(&mut b).await;
                let _ = s.write_all(a).await;
            }
        });
        let m = mw(&format!("http://{addr}/vault"), |_| {}).await;
        for (i, method) in ["GET", "PUT", "GET", "DELETE", "GET"].iter().enumerate() {
            let body: &[u8] = if *method == "PUT" { b"box" } else { b"" };
            let r = m.request(method, "/mw/vault", &[BEARER, ("If-None-Match", "*")], body).await;
            assert_eq!((r.status, r.header("retry-after"), r.header("location")), (503, None, None), "answer {i}");
            assert!(String::from_utf8_lossy(&r.body).contains("VAULT-UNREACHABLE"));
        }
        let line = m.app.vault_forward.as_ref().unwrap().line().unwrap();
        assert!(line.contains("5 failed (5 an answer not the vault's)"), "{line}");
    }

    // What goes on: the token, the conditions, the body, the player's address, the credential and the Access service
    // token on every forward; no other header of the browser's (its own CF-Access-* never); and neither key in the
    // page's answer
    #[tokio::test]
    async fn what_goes_on_and_what_comes_back() {
        let up = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = up.local_addr().unwrap();
        let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<String>();
        tokio::spawn(async move {
            while let Ok((mut s, _)) = up.accept().await {
                let mut b = vec![0u8; 8192];
                let mut n = 0;
                while let Ok(k) = s.read(&mut b[n..]).await {
                    if k == 0 {
                        break;
                    }
                    n += k;
                    let t = String::from_utf8_lossy(&b[..n]).to_string();
                    if let Some(end) = t.find("\r\n\r\n") {
                        let len = t
                            .lines()
                            .find_map(|l| {
                                l.to_ascii_lowercase().strip_prefix("content-length: ").map(|v| v.trim().parse::<usize>().unwrap())
                            })
                            .unwrap_or(0);
                        if n >= end + 4 + len {
                            break;
                        }
                    }
                }
                tx.send(String::from_utf8_lossy(&b[..n]).to_string()).unwrap();
                let _ = s
                    .write_all(
                        b"HTTP/1.1 201 Created\r\netag: \"0000000000000001\"\r\nx-vault-saved: 5\r\nset-cookie: x=1\r\n\
                          x-vault-box: taken\r\ncontent-type: text/plain; charset=utf-8\r\ncontent-length: 6\r\n\r\nSaved\n",
                    )
                    .await;
            }
        });
        let m = mw(&format!("http://{addr}/u2/vault"), |_| {}).await;
        let browser = [
            BEARER,
            ("If-None-Match", "*"),
            ("X-Forwarded-For", "203.0.113.9"),
            ("CF-Access-Client-Id", "the browser's"),
            ("Cookie", "a=b"),
            ("x-vault-box", "a browser's guess"),
            ("Referer", "https://x.example/"),
        ];
        let r = m.request("PUT", "/mw/vault?earlier&x=1", &browser, b"sealed").await;
        assert_eq!(
            (r.status, r.header("etag"), r.header("x-vault-saved"), r.body.as_slice()),
            (201, Some("\"0000000000000001\""), Some("5"), &b"Saved\n"[..])
        );
        assert_eq!((r.header("set-cookie"), r.header("x-vault-box")), (None, None), "only the headers that matter come back");
        let all = format!("{:?}", seen(&r));
        assert!(!all.contains("access-secret") && !all.contains(KEY));
        let sent = rx.recv().await.unwrap();
        let lower = sent.to_ascii_lowercase();
        assert!(sent.starts_with("PUT /u2/vault?earlier HTTP/1.1\r\n"), "{sent}");
        let wants = [
            format!("authorization: bearer {}", T.to_ascii_lowercase()),
            "if-none-match: *".into(),
            "x-vault-client: 203.0.113.9".into(),
            format!("x-vault-box: {}", KEY.to_ascii_lowercase()),
            "cf-access-client-id: access-id.access".into(),
            "cf-access-client-secret: access-secret".into(),
            "content-length: 6".into(),
        ];
        for want in &wants {
            assert!(lower.contains(want.as_str()), "{want} in {sent}");
        }
        assert!(sent.ends_with("\r\n\r\nsealed"));
        for not in ["the browser's", "cookie", "a browser's guess", "referer", "x=1"] {
            assert!(!lower.contains(not), "{not} went on: {sent}");
        }
        assert_eq!(lower.matches("cf-access-client-id").count(), 1);
    }

    #[test]
    fn its_keys() {
        assert!(VaultForward::new("https://x.example/vault", "short".into(), None).is_err());
        assert!(VaultForward::new("ftp://x.example/vault", KEY.into(), None).is_err());
        assert!(VaultForward::new("https://x.example/vault?a", KEY.into(), None).is_err());
        assert!(VaultForward::new("https://x.example/vault", KEY.into(), Some(("a b".into(), "c".into()))).is_err());
        let f = VaultForward::new("https://x.example/vault", KEY.into(), Some(("id".into(), "secret".into()))).unwrap();
        assert_eq!((f.host.as_str(), f.port, f.path.as_str()), ("x.example", 443, "/vault"));
        assert!(!f.target().contains("secret") && !f.target().contains(KEY));
        assert_eq!(f.line(), None);
    }

    // Every length of the vault's answer, and every length of a forwarded request's address and credential: nothing
    // panics (L-181); a cut answer is the 503 of an unreachable vault, a whole one the vault's; a forward the U2 box
    // cannot take is a 400 (no address) or refused (not its credential)
    #[tokio::test]
    async fn every_length_panics_nothing() {
        const WHOLE: &[u8] =
            b"HTTP/1.1 200 OK\r\netag: \"0000000000000001\"\r\nx-vault-saved: 5\r\ncontent-type: application/octet-stream\r\n\
                               x-vault-box: taken\r\ntransfer-encoding: chunked\r\n\r\n4\r\nbox!\r\n3\r\nyes\r\n0\r\n\r\n";
        let up = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = up.local_addr().unwrap();
        let cut = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let c = cut.clone();
        tokio::spawn(async move {
            while let Ok((mut s, _)) = up.accept().await {
                let mut b = [0u8; 4096];
                let _ = s.read(&mut b).await;
                let n = c.load(std::sync::atomic::Ordering::SeqCst);
                let _ = s.write_all(&WHOLE[..n]).await;
            }
        });
        let m = mw(&format!("http://{addr}/vault"), |_| {}).await;
        for n in 0..=WHOLE.len() {
            cut.store(n, std::sync::atomic::Ordering::SeqCst);
            let r = m.request("GET", "/mw/vault", &[BEARER], b"").await;
            if n == WHOLE.len() {
                assert_eq!((r.status, r.body.as_slice(), r.header("etag")), (200, &b"box!yes"[..], Some("\"0000000000000001\"")));
            } else {
                assert!(r.status == 503 || r.status == 200, "{n}: {}", r.status);
            }
        }
        let b = u2().await;
        let addrs =
            ["", "1", "1.2.3", "1.2.3.4", "::1", "[::1]", "fe80::1%x", "x".repeat(300).as_str(), "1.2.3.4, 5.6.7.8"].map(str::to_string);
        for a in &addrs {
            for k in 0..=KEY.len() + 1 {
                let key = format!("{}{}", &KEY[..k.min(KEY.len())], if k > KEY.len() { "x" } else { "" });
                let r = b.request("GET", "/vault", &[BEARER, ("x-vault-box", &key), ("x-vault-client", a)], b"").await;
                if key == KEY {
                    assert!(matches!(r.status, 400 | 404 | 429), "{a:?} {k}: {}", r.status);
                } else {
                    assert_eq!((r.status, r.header("x-vault-box")), (403, Some(REFUSED)), "{a:?} {k}");
                }
            }
        }
    }
}
