//! /vault: keyphrase saves (nfsu2-vault), for this server's own pages only.
//!
//! The page sends its token as `Authorization: Bearer <43 characters of base64url>` (never in the URL: no log or
//! cache sees it); the keyphrase is kept under the token's SHA-256.  What it sends and gets back is the page's
//! encrypted save folder: the server neither reads nor checks it.
//!
//! - `GET /vault`: the current version, its rev as the ETag (`If-None-Match` it: 304), `x-vault-saved` when it was
//!   saved and `x-vault-earlier` when the copy of an earlier day was (none: no such header); 404 when there is no such
//!   keyphrase.  `GET /vault?earlier`: that copy, with its own ETag and `x-vault-saved` (404: none).  Without a token:
//!   401, which is how a page tells a server with keyphrase saves (one without answers 404).
//! - `PUT /vault` with `If-None-Match: *` (a new keyphrase) or `If-Match: "<rev>"` (the version the page has): 201 or
//!   200 with the new ETag and `x-vault-saved`; 412 when that does not hold, with the current ETag and
//!   `x-vault-saved` (none: no such keyphrase); 428 with neither; 413 past 256 KB; 507 when the server's room for them
//!   is full, or its disk nearly; 503 (Retry-After) when the bytes the saves may add a day, all clients together,
//!   are taken: a new keyphrase, or a save that grows one past 32 KB (crates/vault, Limits).
//! - `DELETE /vault`: the keyphrase and both its versions go (404: none).
//!
//! Per client (its budget key, and twice that per site as `Rate` does): 60 reads, then one a second; 60 writes, then
//! one every two seconds; 20 new keyphrases, then 20 a day; and 30 reads of keyphrases there are not, then 30 an hour
//! (checked before the read: a client that guesses keyphrases learns nothing past those).  More gets 429.  The
//! console counts keyphrases and bytes, never names one.

use std::sync::Arc;

use nfsu2_headers::Cache;
use nfsu2_http::{Request, Response};
use nfsu2_util::{Rate, js, log};
use nfsu2_vault::{Cond, Put, Vault, Want, id_of};

use super::{App, Fail, Hdrs, Res, finish, present, send, url};
use crate::options::js_to_number;

pub const READ_BURST: f64 = 60.0;
pub const READ_RATE: f64 = 1.0;
pub const WRITE_BURST: f64 = 60.0;
pub const WRITE_RATE: f64 = 0.5;
pub const NEW_BURST: f64 = 20.0;
pub const NEW_RATE: f64 = 20.0 / 86_400.0;
pub const MISS_BURST: f64 = 30.0;
pub const MISS_RATE: f64 = 30.0 / 3600.0;
/// file operations at once (each on a blocking thread)
pub const IO_AT_ONCE: usize = 32;

/// The keyphrase saves of an App: the store and its limits.
pub struct VaultRoute {
    pub store: Arc<Vault>,
    reads: Rate,
    writes: Rate,
    news: Rate,
    misses: Rate,
    io: tokio::sync::Semaphore,
}

impl VaultRoute {
    pub fn new(store: Arc<Vault>) -> VaultRoute {
        VaultRoute {
            store,
            reads: Rate::new(READ_BURST, READ_RATE),
            writes: Rate::new(WRITE_BURST, WRITE_RATE),
            news: Rate::new(NEW_BURST, NEW_RATE),
            misses: Rate::new(MISS_BURST, MISS_RATE),
            io: tokio::sync::Semaphore::new(IO_AT_ONCE),
        }
    }

    /// the buckets that are full again hold nothing worth keeping (every minute)
    pub fn prune(&self) {
        for r in [&self.reads, &self.writes, &self.news, &self.misses] {
            r.prune();
        }
    }
}

/// `Bearer <43 characters of base64url>`: the token (what its SHA-256 keeps the keyphrase under), or None
fn token(q: &Request) -> Option<&str> {
    let t = q.headers.get("authorization")?.strip_prefix("Bearer ")?;
    (t.len() == 43 && t.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_')).then_some(t)
}

/// `"<16 hex digits>"`, as the ETag carries a rev
fn etag(rev: u64) -> String {
    format!("\"{rev:016x}\"")
}

/// an If-Match's rev: one strong ETag of ours, else None
fn if_match(v: &str) -> Option<u64> {
    let h = js::trim(v).strip_prefix('"')?.strip_suffix('"')?;
    (h.len() == 16).then(|| u64::from_str_radix(h, 16).ok()).flatten()
}

impl App {
    /// /vault (keyphrase saves): see this module's doc
    pub(super) async fn vault(&self, q: &mut Request, s: &mut Response, u: &url::Url) -> Res {
        const CLOSE: &[(&str, &str)] = &[("connection", "close")];
        let Some(v) = &self.vault else { return self.not_found(q, s).await };
        // only this server's own pages: a page of another site must not reach a player's saves through their browser
        let site_ok = q.headers.get("sec-fetch-site").is_none_or(|f| f == "same-origin");
        if !self.own_page(q) || !site_ok {
            return self.plain(q, s, 403, "Forbidden", &[], Cache::Store, true).await;
        }
        let method = q.method.clone();
        if !matches!(method.as_str(), "GET" | "HEAD" | "PUT" | "DELETE") {
            return self
                .plain(q, s, 405, "Method not allowed", &[("allow", "GET, HEAD, PUT, DELETE"), ("connection", "close")], Cache::Store, true)
                .await;
        }
        if method != "PUT" && (js_to_number(q.headers.get("content-length").unwrap_or("0")) > 0.0 || present(q, "transfer-encoding")) {
            return self.plain(q, s, 413, "Too large", CLOSE, Cache::Store, true).await;
        }
        let Some(id) = token(q).map(|t| id_of(t.as_bytes())) else {
            return self.plain(q, s, 401, "A keyphrase's token is needed", &[], Cache::Store, true).await;
        };
        let (_, key, site) = self.client(q);
        let now = nfsu2_util::now_ms();
        let store = v.store.clone();
        match method.as_str() {
            "GET" | "HEAD" => {
                if !v.reads.take(&key, &site) || !v.misses.check(&key, &site) {
                    return self.too_many(q, s).await;
                }
                let want = if u.has_param("earlier") { Want::Earlier } else { Want::Current };
                // a page that has the current version asks with its ETag: the head is enough for the 304
                let inm = q.headers.get("if-none-match").map(str::to_string);
                if want == Want::Current && inm.is_some() {
                    let st = store.clone();
                    let meta = self.vault_io(v, move || st.meta(&id, now)).await?;
                    if let Some(m) = meta
                        && nfsu2_headers::etag_matches(inm.as_deref(), &etag(m.cur.rev))
                    {
                        let mut h = self.headers(q, Cache::Store, true);
                        vault_headers(&mut h, m.cur.rev, m.cur.saved, m.prev.map(|p| p.saved));
                        s.head(304, h.0);
                        return finish(s).await;
                    }
                }
                let got = self.vault_io(v, move || store.get(&id, want, now)).await?;
                let found = got.and_then(|(m, body)| match want {
                    Want::Current => Some((m.cur, m.prev.map(|p| p.saved), body)),
                    Want::Earlier => m.prev.map(|p| (p, None, body)),
                });
                let Some((ver, earlier, body)) = found else {
                    v.misses.take(&key, &site);
                    return self.not_found(q, s).await;
                };
                let mut h = self.headers(q, Cache::Store, true);
                h.set("content-type", "application/octet-stream");
                h.set("content-length", body.len().to_string());
                vault_headers(&mut h, ver.rev, ver.saved, earlier);
                s.head(200, h.0);
                if method != "HEAD" {
                    send(s, &body).await?;
                }
                finish(s).await
            }
            "PUT" => {
                let cond = match (q.headers.get("if-match"), q.headers.get("if-none-match")) {
                    (Some(m), _) => match if_match(m) {
                        Some(r) => Cond::Rev(r),
                        None => return self.plain(q, s, 412, "Changed elsewhere", &[], Cache::Store, true).await,
                    },
                    (None, Some(n)) if js::trim(n) == "*" => Cond::New,
                    _ => return self.plain(q, s, 428, "If-Match or If-None-Match: * is needed", &[], Cache::Store, true).await,
                };
                if !v.writes.take(&key, &site) || (cond == Cond::New && !v.news.take(&key, &site)) {
                    return self.too_many(q, s).await;
                }
                let max = store.options().max_body;
                if js_to_number(q.headers.get("content-length").unwrap_or("0")) > max as f64 {
                    return self.plain(q, s, 413, "Too large", CLOSE, Cache::Store, true).await;
                }
                let body = match q.read_body(max).await {
                    Ok(b) => b,
                    Err(nfsu2_http::BodyError::TooLarge) => return self.plain(q, s, 413, "Too large", CLOSE, Cache::Store, true).await,
                    Err(nfsu2_http::BodyError::Broken) => {
                        s.destroy();
                        return Err(Fail::Gone);
                    }
                };
                let put = self.vault_io(v, move || store.put(&id, cond, &body, now)).await?;
                let mut h = self.headers(q, Cache::Store, true);
                let (code, text) = match put {
                    Put::Saved { meta, created } => {
                        vault_headers(&mut h, meta.cur.rev, meta.cur.saved, meta.prev.map(|p| p.saved));
                        if created { (201, "Saved") } else { (200, "Saved") }
                    }
                    Put::Conflict(m) => {
                        if let Some(m) = m {
                            vault_headers(&mut h, m.cur.rev, m.cur.saved, m.prev.map(|p| p.saved));
                        }
                        (412, "Changed elsewhere")
                    }
                    Put::TooLarge => (413, "Too large"),
                    Put::Full => {
                        let (n, bytes, _) = v.store.usage();
                        log::limited(
                            "vault",
                            "full",
                            "",
                            &format!("[vault] refused a save: the room for keyphrase saves is full ({n} keyphrases, {bytes} bytes)"),
                            1,
                        );
                        (507, "The server has no room for more saves")
                    }
                    Put::DiskLow => {
                        let o = v.store.options();
                        log::limited(
                            "vault",
                            "disk",
                            "",
                            &format!(
                                "[vault] refused a save: the disk has less than {} bytes or {} files free",
                                o.min_free, o.min_free_files
                            ),
                            1,
                        );
                        (507, "The server has no room for more saves")
                    }
                    Put::Busy => {
                        log::limited(
                            "vault",
                            "busy",
                            "",
                            &format!(
                                "[vault] refused a save: the {} a day the saves may add are taken for now (all clients \
                                 together)",
                                super::run::size(v.store.options().per_day)
                            ),
                            1,
                        );
                        h.set("retry-after", "3600");
                        (503, "The server takes no more new saves for now")
                    }
                };
                let body = format!("{text}\n");
                h.set("content-type", "text/plain; charset=utf-8");
                h.set("content-length", body.len().to_string());
                s.head(code, h.0);
                send(s, body.as_bytes()).await?;
                finish(s).await
            }
            _ => {
                if !v.writes.take(&key, &site) {
                    return self.too_many(q, s).await;
                }
                if self.vault_io(v, move || store.delete(&id)).await? {
                    // its claimed name is free (names.rs)
                    self.release_names(&id);
                    self.plain(q, s, 200, "Deleted", &[], Cache::Store, true).await
                } else {
                    self.not_found(q, s).await
                }
            }
        }
    }

    async fn too_many(&self, q: &Request, s: &mut Response) -> Res {
        self.plain(q, s, 429, "Too many requests", &[("retry-after", "60")], Cache::Store, true).await
    }

    /// a file operation on a blocking thread, IO_AT_ONCE of them at most; an error is the 500's (no path in it)
    async fn vault_io<T: Send + 'static>(
        &self,
        v: &VaultRoute,
        f: impl FnOnce() -> std::io::Result<T> + Send + 'static,
    ) -> Result<T, Fail> {
        let _turn = v.io.acquire().await.map_err(|_| Fail::Error("keyphrase saves: closed".into()))?;
        match tokio::task::spawn_blocking(f).await {
            Ok(Ok(t)) => Ok(t),
            Ok(Err(e)) => Err(Fail::Error(format!("keyphrase saves: {e}"))),
            Err(e) => Err(Fail::Error(format!("keyphrase saves: {e}"))),
        }
    }
}

/// a version's ETag and time, and when the copy of an earlier day was saved
fn vault_headers(h: &mut Hdrs, rev: u64, saved: i64, earlier: Option<i64>) {
    h.set("etag", etag(rev));
    h.set("x-vault-saved", saved.to_string());
    if let Some(e) = earlier {
        h.set("x-vault-earlier", e.to_string());
    }
}

#[cfg(test)]
mod tests {
    use super::super::testkit::Kit;
    use super::*;

    const TOKEN: &str = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    const BEARER: (&str, &str) = ("Authorization", "Bearer AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");

    fn bearer(t: &str) -> String {
        format!("Bearer {t}")
    }

    #[tokio::test]
    async fn a_keyphrase_saved_read_changed_and_deleted() {
        let kit = Kit::start(&[], |_| {}).await;
        // a server with keyphrase saves answers a request without a token with 401 (how the page tells)
        assert_eq!(kit.request("GET", "/vault", &[], b"").await.status, 401);
        assert_eq!(kit.request("GET", "/vault", &[BEARER], b"").await.status, 404);
        assert_eq!(kit.request("PUT", "/vault", &[BEARER], b"box").await.status, 428);
        let a = kit.request("PUT", "/vault", &[BEARER, ("If-None-Match", "*")], b"box one").await;
        assert_eq!(a.status, 201);
        let tag1 = a.header("etag").unwrap().to_string();
        assert_eq!(a.header("cache-control"), Some("private, no-store, no-transform"));
        let saved: i64 = a.header("x-vault-saved").unwrap().parse().unwrap();
        assert!((saved - nfsu2_util::now_ms()).abs() < 10_000);
        let g = kit.request("GET", "/vault", &[BEARER], b"").await;
        assert_eq!((g.status, g.body.as_slice(), g.header("etag")), (200, &b"box one"[..], Some(tag1.as_str())));
        assert_eq!(g.header("content-type"), Some("application/octet-stream"));
        assert_eq!(g.header("x-vault-earlier"), None);
        let h = kit.request("GET", "/vault", &[BEARER, ("If-None-Match", &tag1)], b"").await;
        assert_eq!((h.status, h.body.len()), (304, 0));
        // a second new one, or the wrong version: 412, with what is there
        let c = kit.request("PUT", "/vault", &[BEARER, ("If-None-Match", "*")], b"other").await;
        assert_eq!((c.status, c.header("etag")), (412, Some(tag1.as_str())));
        let c = kit.request("PUT", "/vault", &[BEARER, ("If-Match", "\"0000000000000001\"")], b"other").await;
        assert_eq!((c.status, c.header("etag")), (412, Some(tag1.as_str())));
        assert_eq!(kit.request("PUT", "/vault", &[BEARER, ("If-Match", "nonsense")], b"other").await.status, 412);
        let u = kit.request("PUT", "/vault", &[BEARER, ("If-Match", &tag1)], b"box two").await;
        assert_eq!(u.status, 200);
        let tag2 = u.header("etag").unwrap().to_string();
        assert_ne!(tag1, tag2);
        assert_eq!(kit.request("GET", "/vault", &[BEARER, ("If-None-Match", &tag1)], b"").await.body, b"box two");
        // the same day: no earlier copy
        assert_eq!(kit.request("GET", "/vault?earlier", &[BEARER], b"").await.status, 404);
        // another keyphrase is another file; none of them names a token
        let other = bearer(&"B".repeat(43));
        assert_eq!(kit.request("GET", "/vault", &[("Authorization", &other)], b"").await.status, 404);
        let hex: String = id_of(TOKEN.as_bytes()).iter().map(|b| format!("{b:02x}")).collect();
        let file = kit.path("state/vault").join(&hex[..3]);
        assert_eq!(std::fs::read_dir(&file).unwrap().count(), 1);
        assert_eq!(kit.request("DELETE", "/vault", &[BEARER], b"").await.status, 200);
        assert_eq!(kit.request("DELETE", "/vault", &[BEARER], b"").await.status, 404);
        assert_eq!(kit.request("GET", "/vault", &[BEARER], b"").await.status, 404);
        assert_eq!(kit.app.vault.as_ref().unwrap().store.usage().0, 0);
    }

    #[tokio::test]
    async fn the_copy_of_an_earlier_day() {
        let kit = Kit::start(&[], |_| {}).await;
        let store = kit.app.vault.as_ref().unwrap().store.clone();
        let day_before = nfsu2_util::now_ms() - 2 * nfsu2_vault::DAY_MS;
        let Put::Saved { meta, .. } = store.put(&id_of(TOKEN.as_bytes()), Cond::New, b"old", day_before).unwrap() else { panic!() };
        let u = kit.request("PUT", "/vault", &[BEARER, ("If-Match", &etag(meta.cur.rev))], b"new").await;
        assert_eq!((u.status, u.header("x-vault-earlier")), (200, Some(day_before.to_string().as_str())));
        let g = kit.request("GET", "/vault", &[BEARER], b"").await;
        assert_eq!((g.body.as_slice(), g.header("x-vault-earlier")), (&b"new"[..], Some(day_before.to_string().as_str())));
        let e = kit.request("GET", "/vault?earlier", &[BEARER], b"").await;
        assert_eq!((e.status, e.body.as_slice()), (200, &b"old"[..]));
        assert_eq!(
            (e.header("etag"), e.header("x-vault-saved")),
            (Some(etag(meta.cur.rev).as_str()), Some(day_before.to_string().as_str()))
        );
        assert_eq!(e.header("x-vault-earlier"), None);
    }

    #[tokio::test]
    async fn only_this_servers_own_pages_and_whole_requests() {
        let kit = Kit::start(&[], |_| {}).await;
        for h in [
            ("Origin", "http://evil.example"),
            ("Origin", "null"),
            ("Sec-Fetch-Site", "cross-site"),
            ("Sec-Fetch-Site", "same-site"),
            ("Sec-Fetch-Site", "none"),
        ] {
            assert_eq!(kit.request("GET", "/vault", &[BEARER, h], b"").await.status, 403, "{h:?}");
            assert_eq!(kit.request("PUT", "/vault", &[BEARER, h, ("If-None-Match", "*")], b"x").await.status, 403, "{h:?}");
        }
        let own = [BEARER, ("Origin", "http://localhost"), ("Sec-Fetch-Site", "same-origin")];
        assert_eq!(kit.request("GET", "/vault", &own, b"").await.status, 404);
        for t in ["Bearer short", &bearer(&"A".repeat(44)), &bearer(&format!("{}+", "A".repeat(42))), &format!("Basic {TOKEN}"), TOKEN] {
            assert_eq!(kit.request("GET", "/vault", &[("Authorization", t)], b"").await.status, 401, "{t}");
        }
        let p = kit.request("POST", "/vault", &[BEARER], b"x").await;
        assert_eq!((p.status, p.header("allow")), (405, Some("GET, HEAD, PUT, DELETE")));
        assert_eq!(kit.request("GET", "/vault", &[BEARER], b"body").await.status, 413);
        // refused by its Content-Length, before the body is read (so none is sent here)
        let over = ((256 << 10) + 1).to_string();
        assert_eq!(kit.request("PUT", "/vault", &[BEARER, ("If-None-Match", "*"), ("Content-Length", &over)], b"").await.status, 413);
        assert_eq!(kit.request("PUT", "/vault", &[BEARER, ("If-None-Match", "*")], &vec![1u8; 256 << 10]).await.status, 201);
        // HEAD: the headers alone
        let h = kit.request("HEAD", "/vault", &[BEARER], b"").await;
        assert_eq!((h.status, h.header("content-length"), h.body.len()), (200, Some("262144"), 0));
        assert!(super::super::ROBOTS_TXT.contains("Disallow: /vault\n"));
    }

    #[tokio::test]
    async fn guesses_and_new_keyphrases_are_limited() {
        let kit = Kit::start(&[], |_| {}).await;
        let t = |i: usize| bearer(&format!("{i:0>43}"));
        // 20 new keyphrases from one client, then 429
        for i in 0..20 {
            assert_eq!(kit.request("PUT", "/vault", &[("Authorization", &t(i)), ("If-None-Match", "*")], b"x").await.status, 201, "{i}");
        }
        let r = kit.request("PUT", "/vault", &[("Authorization", &t(20)), ("If-None-Match", "*")], b"x").await;
        assert_eq!((r.status, r.header("retry-after")), (429, Some("60")));
        // 30 reads of keyphrases there are not, then even one that is there is refused
        for i in 100..130 {
            assert_eq!(kit.request("GET", "/vault", &[("Authorization", &t(i))], b"").await.status, 404, "{i}");
        }
        assert_eq!(kit.request("GET", "/vault", &[("Authorization", &t(0))], b"").await.status, 429);
    }

    #[tokio::test]
    async fn none_with_vault_gb_0() {
        let kit = Kit::start(&["--vault-gb", "0"], |_| {}).await;
        assert!(kit.app.vault.is_none());
        assert_eq!(kit.request("GET", "/vault", &[], b"").await.status, 404);
        assert_eq!(kit.request("PUT", "/vault", &[BEARER, ("If-None-Match", "*")], b"x").await.status, 404);
        assert!(!kit.path("state/vault").exists());
    }

    #[test]
    fn etags_and_if_match() {
        assert_eq!(etag(0x00ab), "\"00000000000000ab\"");
        assert_eq!(if_match(" \"00000000000000ab\" "), Some(0xab));
        for bad in ["*", "W/\"00000000000000ab\"", "\"ab\"", "00000000000000ab", "\"0000000000000xab\"", "\"00000000000000ab\", \"1\""] {
            assert_eq!(if_match(bad), None, "{bad}");
        }
    }
}
