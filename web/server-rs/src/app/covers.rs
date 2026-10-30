//! /covers: the hand-over store (nfsu2-vault `covers`), Underground 2's only, and only where its routes are under a
//! base: on the address it shares with Most Wanted (nfsrevived.xyz/u2/covers), where its old address's pages hand
//! their players' covers over.  At the root (the old address itself, this machine's names as they are) there is no
//! such route, and a server none of whose names has a base keeps no store (`here`).
//!
//! The old address's page writes a player's covers once, from its own origin (CORS), and the new address's page reads
//! them and deletes them once it took them in:
//! - `PUT /covers` with `If-None-Match: *` and `Authorization: Bearer <43 characters of base64url>` (the move's token,
//!   never in the URL): 201; 412 when an entry is there already; 428 without If-None-Match: *; 401 without a token;
//!   413 past the cap of one entry; 507 when the store is full or the disk nearly; 429 past the client's writes; 503
//!   while BODIES_AT_ONCE other writes are under way.  From this server's own pages, or from the old address's
//!   (game.rs `Handover::allows`: exactly its origin, or on this machine's localhost exactly 127.0.0.1's, after an
//!   OPTIONS preflight); an answer to the old address carries `Access-Control-Allow-Origin` with its origin, never
//!   `*` and never credentials.
//! - `GET` (and `HEAD`) `/covers` with the token: the bytes, or 404; `DELETE /covers`: 200, or 404.  This server's own
//!   pages only: no CORS on them, so the old address's page can drop covers off and do nothing else.
//! - `OPTIONS /covers`: the preflight of the old address's PUT: 204 with `Access-Control-Allow-Origin`,
//!   `-Allow-Methods: PUT` and `-Allow-Headers: authorization, content-type, if-none-match`.
//!
//! Every answer here says `Vary: Origin`.  A request from any other origin gets 403 with no CORS header at all.  Per
//! client (its budget key, and twice that per site as `Rate` does): 6 writes, then 6 a day; 60 reads, then one a
//! second; 30 reads of entries there are not, then 30 an hour (checked before the read).  The console counts the
//! entries and their bytes (`[move]`), never names one.

use std::sync::Arc;

use nfsu2_headers::{Cache, HostCheck};
use nfsu2_http::{Request, Response};
use nfsu2_util::{Rate, js, log};
use nfsu2_vault::covers::{CoverOptions, CoverStore, Store};
use nfsu2_vault::id_of;

use super::{App, Fail, Hdrs, Res, finish, present, send};
use crate::options::{Options, js_to_number};

pub const WRITE_BURST: f64 = 6.0;
pub const WRITE_RATE: f64 = 6.0 / 86_400.0;
pub const READ_BURST: f64 = 60.0;
pub const READ_RATE: f64 = 1.0;
pub const MISS_BURST: f64 = 30.0;
pub const MISS_RATE: f64 = 30.0 / 3600.0;
/// file operations at once (each on a blocking thread)
pub const IO_AT_ONCE: usize = 8;
/// writes whose bodies are held at once (each up to an entry's cap, until it is written): past them a write is told to
/// come back later (503), never queued
pub const BODIES_AT_ONCE: usize = 8;
/// the request headers the old address's PUT may send (its preflight's Access-Control-Request-Headers)
pub const ALLOW_HEADERS: &str = "authorization, content-type, if-none-match";
/// how long a browser may keep the preflight's answer (s)
pub const PREFLIGHT_MAX_AGE: &str = "600";

/// The hand-over store of an App: the store and its limits.
pub struct CoversRoute {
    pub store: Arc<CoverStore>,
    writes: Rate,
    reads: Rate,
    misses: Rate,
    io: tokio::sync::Semaphore,
    bodies: tokio::sync::Semaphore,
}

impl CoversRoute {
    pub fn new(store: Arc<CoverStore>) -> CoversRoute {
        CoversRoute {
            store,
            writes: Rate::new(WRITE_BURST, WRITE_RATE),
            reads: Rate::new(READ_BURST, READ_RATE),
            misses: Rate::new(MISS_BURST, MISS_RATE),
            io: tokio::sync::Semaphore::new(IO_AT_ONCE),
            bodies: tokio::sync::Semaphore::new(BODIES_AT_ONCE),
        }
    }

    /// the buckets that are full again hold nothing worth keeping (every minute)
    pub fn prune(&self) {
        for r in [&self.writes, &self.reads, &self.misses] {
            r.prune();
        }
    }
}

/// Whether this server keeps the hand-over store: the game has a move from an old address (Underground 2), and one of
/// the names the server answers has its routes under a base (the address both games share).
pub fn here(o: &Options) -> bool {
    o.game.handover.is_some() && {
        let names = HostCheck::new(&o.hosts);
        o.bases.all(|n| names.check(Some(n))).iter().any(|b| !b.is_empty())
    }
}

/// The store in the state folder's `covers`, when this server keeps one (`here`).
pub fn open(o: &Options) -> std::io::Result<Option<Arc<CoverStore>>> {
    if !here(o) {
        return Ok(None);
    }
    let dir = std::path::PathBuf::from(crate::paths::join(&o.state, "covers"));
    Ok(Some(Arc::new(CoverStore::open(CoverOptions::defaults(dir))?)))
}

/// `Bearer <43 characters of base64url>`: the token, or None
fn token(q: &Request) -> Option<&str> {
    let t = q.headers.get("authorization")?.strip_prefix("Bearer ")?;
    (t.len() == 43 && t.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-' || c == b'_')).then_some(t)
}

/// the request has a body (one a route without one refuses)
fn has_body(q: &Request) -> bool {
    js_to_number(q.headers.get("content-length").unwrap_or("0")) > 0.0 || present(q, "transfer-encoding")
}

/// the usual headers, with the CORS answer for the old address's page (`old`: its origin) and Vary: Origin
fn covers_headers(app: &App, q: &Request, old: Option<&str>) -> Hdrs {
    let mut h = app.headers(q, Cache::Store, true);
    if let Some(o) = old {
        h.set("access-control-allow-origin", o);
    }
    h.set("vary", "Origin");
    h
}

impl App {
    /// /covers (the hand-over store): see this module's doc
    pub(super) async fn covers(&self, q: &mut Request, s: &mut Response) -> Res {
        let (Some(c), Some(m)) = (&self.covers, self.o.game.handover) else { return self.not_found(q, s).await };
        let method = q.method.clone();
        // the old address's page (exactly its origin), or this server's own (same-origin, as the vault takes them)
        let old = q.headers.get("origin").filter(|o| m.allows(q.headers.get("host").unwrap_or(""), o)).map(str::to_string);
        let own = old.is_none() && self.own_page(q) && q.headers.get("sec-fetch-site").is_none_or(|f| f == "same-origin");
        let old = old.as_deref();
        if method == "OPTIONS" {
            let wants_put = q.headers.get("access-control-request-method").is_some_and(|v| js::trim(v) == "PUT");
            let headers_ok = q.headers.get("access-control-request-headers").is_none_or(|v| {
                v.split(',')
                    .map(|h| js::trim(h).to_ascii_lowercase())
                    .filter(|h| !h.is_empty())
                    .all(|h| ALLOW_HEADERS.split(", ").any(|a| a == h))
            });
            if old.is_none() || !wants_put || !headers_ok {
                return self.covers_plain(q, s, None, 403, "Forbidden", &[]).await;
            }
            if has_body(q) {
                return self.covers_plain(q, s, None, 413, "Too large", &[("connection", "close")]).await;
            }
            let mut h = covers_headers(self, q, old);
            h.set("access-control-allow-methods", "PUT");
            h.set("access-control-allow-headers", ALLOW_HEADERS);
            h.set("access-control-max-age", PREFLIGHT_MAX_AGE);
            h.set("content-length", "0");
            s.head(204, h.0);
            return finish(s).await;
        }
        // the old address's page may write, and nothing else; any other origin nothing at all
        if !(own || (old.is_some() && method == "PUT")) {
            return self.covers_plain(q, s, None, 403, "Forbidden", &[]).await;
        }
        if !matches!(method.as_str(), "GET" | "HEAD" | "PUT" | "DELETE") {
            let allow = [("allow", "GET, HEAD, PUT, DELETE, OPTIONS"), ("connection", "close")];
            return self.covers_plain(q, s, None, 405, "Method not allowed", &allow).await;
        }
        if method != "PUT" && has_body(q) {
            return self.covers_plain(q, s, None, 413, "Too large", &[("connection", "close")]).await;
        }
        let Some(id) = token(q).map(|t| id_of(t.as_bytes())) else {
            return self.covers_plain(q, s, old, 401, "The move's token is needed", &[]).await;
        };
        let (_, key, site) = self.client(q);
        let now = nfsu2_util::now_ms();
        let store = c.store.clone();
        match method.as_str() {
            "PUT" => {
                if q.headers.get("if-none-match").is_none_or(|v| js::trim(v) != "*") {
                    return self.covers_plain(q, s, old, 428, "If-None-Match: * is needed", &[]).await;
                }
                if !c.writes.take(&key, &site) {
                    c.store.refused();
                    return self.covers_plain(q, s, old, 429, "Too many requests", &[("retry-after", "3600")]).await;
                }
                let max = store.options().max_entry;
                if js_to_number(q.headers.get("content-length").unwrap_or("0")) > max as f64 {
                    c.store.refused();
                    return self.covers_plain(q, s, old, 413, "Too large", &[("connection", "close")]).await;
                }
                // a body is held until it is written: BODIES_AT_ONCE of them, else later (no queue of them)
                let Ok(_turn) = c.bodies.try_acquire() else {
                    c.store.refused();
                    let later = [("retry-after", "60"), ("connection", "close")];
                    return self.covers_plain(q, s, old, 503, "Busy: try again later", &later).await;
                };
                let body = match q.read_body(max).await {
                    Ok(b) => b,
                    Err(nfsu2_http::BodyError::TooLarge) => {
                        c.store.refused();
                        return self.covers_plain(q, s, old, 413, "Too large", &[("connection", "close")]).await;
                    }
                    Err(nfsu2_http::BodyError::Broken) => {
                        s.destroy();
                        return Err(Fail::Gone);
                    }
                };
                let put = self.covers_io(c, move || store.put(&id, &body, now)).await?;
                let (code, text) = match put {
                    Store::Stored => (201, "Stored"),
                    Store::Exists => (412, "There already"),
                    Store::TooLarge => (413, "Too large"),
                    Store::Full | Store::DiskLow => {
                        let (n, bytes) = c.store.usage();
                        let why = if put == Store::Full { "the store is full" } else { "the disk is nearly full" };
                        log::limited("move", "full", "", &format!("[move] refused covers: {why} ({n} entries, {bytes} bytes)"), 1);
                        (507, "The server has no room for the covers")
                    }
                };
                self.covers_plain(q, s, old, code, text, &[]).await
            }
            "GET" | "HEAD" => {
                if !c.reads.take(&key, &site) || !c.misses.check(&key, &site) {
                    return self.covers_plain(q, s, None, 429, "Too many requests", &[("retry-after", "60")]).await;
                }
                let Some(body) = self.covers_io(c, move || store.get(&id, now)).await? else {
                    c.misses.take(&key, &site);
                    return self.covers_plain(q, s, None, 404, "Not found", &[]).await;
                };
                let mut h = covers_headers(self, q, None);
                h.set("content-type", "application/octet-stream");
                h.set("content-length", body.len().to_string());
                s.head(200, h.0);
                if method != "HEAD" {
                    send(s, &body).await?;
                }
                finish(s).await
            }
            _ => {
                if !c.writes.take(&key, &site) {
                    return self.covers_plain(q, s, None, 429, "Too many requests", &[("retry-after", "60")]).await;
                }
                if self.covers_io(c, move || store.delete(&id)).await? {
                    self.covers_plain(q, s, None, 200, "Deleted", &[]).await
                } else {
                    self.covers_plain(q, s, None, 404, "Not found", &[]).await
                }
            }
        }
    }

    /// a short text answer of /covers: its CORS headers for the old address's page (`old`: its origin), Vary: Origin
    /// on every one
    async fn covers_plain(&self, q: &Request, s: &mut Response, old: Option<&str>, code: u16, text: &str, extra: &[(&str, &str)]) -> Res {
        let body = format!("{text}\n");
        let mut h = covers_headers(self, q, old);
        h.set("content-type", "text/plain; charset=utf-8");
        h.set("content-length", body.len().to_string());
        for (k, v) in extra {
            h.set(k, *v);
        }
        s.head(code, h.0);
        if q.method != "HEAD" {
            send(s, body.as_bytes()).await?;
        }
        finish(s).await
    }

    /// a file operation on a blocking thread, IO_AT_ONCE of them at most; an error is the 500's (no path in it)
    async fn covers_io<T: Send + 'static>(
        &self,
        c: &CoversRoute,
        f: impl FnOnce() -> std::io::Result<T> + Send + 'static,
    ) -> Result<T, Fail> {
        let _turn = c.io.acquire().await.map_err(|_| Fail::Error("hand-over store: closed".into()))?;
        match tokio::task::spawn_blocking(f).await {
            Ok(Ok(t)) => Ok(t),
            Ok(Err(e)) => Err(Fail::Error(format!("hand-over store: {e}"))),
            Err(e) => Err(Fail::Error(format!("hand-over store: {e}"))),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::testkit::{Answer, Kit};

    const TOKEN: &str = "MOVEMOVEMOVEMOVEMOVEMOVEMOVEMOVEMOVEMOVE-_0";
    const OLD: &str = "https://nfsu2.xyz";
    const NEW: &str = "nfsrevived.xyz";

    fn bearer(t: &str) -> String {
        format!("Bearer {t}")
    }

    /// no CORS answer at all
    fn no_cors(a: &Answer) -> bool {
        a.headers.iter().all(|(k, _)| !k.starts_with("access-control-"))
    }

    async fn ug2(args: &[&str]) -> Kit {
        Kit::start(args, |root| {
            std::fs::write(root.join("repo/web/dist/main.js"), b"// the page").unwrap();
        })
        .await
    }

    // The store is where Underground 2's routes are under a base (the shared address's /u2/covers) and nowhere else:
    // not at the root of the old address, of this machine's names or of the shared one; a server none of whose names
    // has a base (the old address's box, this machine) keeps none, and neither does Most Wanted's
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn the_store_is_under_the_shared_addresss_base_only() {
        let kit = ug2(&["--host", "nfsu2.xyz,nfsrevived.xyz"]).await;
        assert!(kit.app.covers.is_some() && kit.path("state/covers").is_dir());
        let a = kit.request_to(NEW, "GET", "/u2/covers", &[], b"").await;
        assert_eq!((a.status, a.header("vary")), (401, Some("Origin")));
        for (host, p) in
            [(NEW, "/covers"), ("nfsu2.xyz", "/covers"), ("nfsu2.xyz", "/u2/covers"), ("localhost", "/covers"), ("127.0.0.1", "/covers")]
        {
            // as any path there that is no route: 404, and 405 for a method but GET and HEAD
            assert_eq!(kit.request_to(host, "GET", p, &[], b"").await.status, 404, "{host} {p}");
            let o = kit.request_to(host, "OPTIONS", p, &[("Origin", OLD), ("Access-Control-Request-Method", "PUT")], b"").await;
            assert!(o.status == 405 && no_cors(&o), "{host} {p}: {}", o.status);
        }
        for args in [&[][..], &["--host", "nfsu2.xyz"][..]] {
            let k = ug2(args).await;
            assert!(k.app.covers.is_none(), "{args:?}");
            assert!(!k.path("state/covers").exists(), "{args:?}");
            assert_eq!(k.request_to("localhost", "PUT", "/covers", &[("If-None-Match", "*")], b"x").await.status, 405);
        }
        let mw = Kit::start(&["--game", "mw", "--host", NEW], |root| {
            std::fs::write(root.join("game/speed.exe"), b"MZ").unwrap();
            std::fs::create_dir_all(root.join("repo/web/dist-mw")).unwrap();
            std::fs::write(root.join("repo/web/dist-mw/index.html"), b"<!doctype html>").unwrap();
        })
        .await;
        assert!(mw.app.covers.is_none());
        assert_eq!(mw.request_to(NEW, "GET", "/mw/covers", &[], b"").await.status, 404);
    }

    // The old address's page drops covers off, and does nothing else: its preflight gets the exact CORS answer, its
    // PUT is taken once (the answer readable by it), its GET, HEAD and DELETE are refused without CORS, and so is any
    // other origin, also a look-alike; the new address's page reads them and deletes them; the vault answers the old
    // address no CORS either
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn the_old_address_drops_covers_off_and_the_new_one_takes_them_in() {
        let kit = ug2(&["--host", "nfsu2.xyz,nfsrevived.xyz"]).await;
        let auth = bearer(TOKEN);
        let pre = [
            ("Origin", OLD),
            ("Access-Control-Request-Method", "PUT"),
            ("Access-Control-Request-Headers", "authorization,content-type,if-none-match"),
        ];
        let p = kit.request_to(NEW, "OPTIONS", "/u2/covers", &pre, b"").await;
        assert_eq!(p.status, 204);
        assert_eq!(
            (p.header("access-control-allow-origin"), p.header("access-control-allow-methods"), p.header("access-control-allow-headers")),
            (Some(OLD), Some("PUT"), Some("authorization, content-type, if-none-match"))
        );
        assert_eq!((p.header("vary"), p.header("access-control-allow-credentials"), p.body.len()), (Some("Origin"), None, 0));
        // anyone else: no CORS, 403
        for o in [
            "http://nfsu2.xyz",
            "https://nfsu2.xyz.evil.example",
            "https://evil-nfsu2.xyz",
            "https://www.nfsu2.xyz",
            "https://nfsu2.xyz:8443",
            "null",
            "http://127.0.0.1:9780",
            "https://nfsrevived.xyz",
        ] {
            let p = kit.request_to(NEW, "OPTIONS", "/u2/covers", &[("Origin", o), pre[1], pre[2]], b"").await;
            assert!(p.status == 403 && no_cors(&p), "{o}: {}", p.status);
            if o == "https://nfsrevived.xyz" {
                continue; // (the new address's own page: same-origin, no preflight)
            }
            let w =
                kit.request_to(NEW, "PUT", "/u2/covers", &[("Origin", o), ("Authorization", &auth), ("If-None-Match", "*")], b"x").await;
            assert!(w.status == 403 && no_cors(&w), "{o}: {}", w.status);
        }
        // the old address's preflight for anything but its PUT
        for (m, h) in [("GET", "authorization"), ("DELETE", "authorization"), ("PUT", "authorization, x-other"), ("PUT", "cookie")] {
            let p = kit
                .request_to(
                    NEW,
                    "OPTIONS",
                    "/u2/covers",
                    &[("Origin", OLD), ("Access-Control-Request-Method", m), ("Access-Control-Request-Headers", h)],
                    b"",
                )
                .await;
            assert!(p.status == 403 && no_cors(&p), "{m} {h}: {}", p.status);
        }
        // its PUT: once
        let put = [("Origin", OLD), ("Authorization", auth.as_str()), ("If-None-Match", "*"), ("Content-Type", "application/octet-stream")];
        let w = kit.request_to(NEW, "PUT", "/u2/covers", &put, b"sealed covers").await;
        assert_eq!((w.status, w.header("access-control-allow-origin"), w.header("vary")), (201, Some(OLD), Some("Origin")));
        let w = kit.request_to(NEW, "PUT", "/u2/covers", &put, b"again").await;
        assert_eq!((w.status, w.header("access-control-allow-origin")), (412, Some(OLD)));
        let w = kit.request_to(NEW, "PUT", "/u2/covers", &put[..2], b"x").await;
        assert_eq!((w.status, w.header("access-control-allow-origin")), (428, Some(OLD)));
        let w = kit.request_to(NEW, "PUT", "/u2/covers", &[("Origin", OLD), ("If-None-Match", "*")], b"x").await;
        assert_eq!((w.status, w.header("access-control-allow-origin")), (401, Some(OLD)));
        // and nothing else
        for m in ["GET", "HEAD", "DELETE", "POST"] {
            let a = kit.request_to(NEW, m, "/u2/covers", &[("Origin", OLD), ("Authorization", &auth)], b"").await;
            assert!(a.status == 403 && no_cors(&a), "{m}: {}", a.status);
        }
        for (m, body) in [("GET", &b""[..]), ("PUT", &b"x"[..]), ("DELETE", &b""[..])] {
            let a = kit.request_to(NEW, m, "/u2/vault", &[("Origin", OLD), ("Authorization", &auth), ("If-None-Match", "*")], body).await;
            assert!(a.status == 403 && no_cors(&a), "/u2/vault {m}: {}", a.status);
        }
        // the new address's page: reads them, then deletes them (another site's request without an Origin: no)
        let a = kit.request_to(NEW, "GET", "/u2/covers", &[("Authorization", &auth), ("Sec-Fetch-Site", "cross-site")], b"").await;
        assert_eq!(a.status, 403);
        let a = kit.request_to(NEW, "GET", "/u2/covers", &[("Origin", "https://nfsrevived.xyz"), ("Authorization", &auth)], b"").await;
        assert_eq!((a.status, a.body.as_slice(), a.header("content-type")), (200, &b"sealed covers"[..], Some("application/octet-stream")));
        assert!(no_cors(&a));
        let other = bearer(&"B".repeat(43));
        assert_eq!(kit.request_to(NEW, "GET", "/u2/covers", &[("Authorization", &other)], b"").await.status, 404);
        assert_eq!(kit.request_to(NEW, "DELETE", "/u2/covers", &[("Authorization", &auth)], b"").await.status, 200);
        assert_eq!(kit.request_to(NEW, "DELETE", "/u2/covers", &[("Authorization", &auth)], b"").await.status, 404);
        assert_eq!(kit.request_to(NEW, "GET", "/u2/covers", &[("Authorization", &auth)], b"").await.status, 404);
        let k = kit.app.covers.as_ref().unwrap().store.counts();
        assert_eq!((k.stored, k.imported, k.expired, k.refused), (1, 1, 0, 1));
        assert_eq!(
            super::super::run::move_line(&kit.app.covers.as_ref().unwrap().store),
            "[move] covers: 0 entries, 0.0 MB held; 1 stored, 1 imported, 0 expired, 1 refused"
        );
    }

    // This machine's layout (the tests' two addresses): with localhost's routes under /u2, its store answers the old
    // address's stand-in, 127.0.0.1 at any port, and nothing else; on the shared address's own name, 127.0.0.1 gets
    // nothing
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn on_this_machine_localhost_answers_127_0_0_1() {
        let kit = ug2(&["--host", "nfsrevived.xyz", "--base-path", "localhost=/u2"]).await;
        let auth = bearer(TOKEN);
        for o in ["http://127.0.0.1:9780", "http://127.0.0.1"] {
            let p = kit
                .request_to("localhost:9781", "OPTIONS", "/u2/covers", &[("Origin", o), ("Access-Control-Request-Method", "PUT")], b"")
                .await;
            assert_eq!((p.status, p.header("access-control-allow-origin")), (204, Some(o)), "{o}");
        }
        for o in [OLD, "http://localhost:9781", "https://127.0.0.1:9780", "http://127.0.0.1:9780.evil"] {
            let p = kit
                .request_to("localhost:9781", "OPTIONS", "/u2/covers", &[("Origin", o), ("Access-Control-Request-Method", "PUT")], b"")
                .await;
            assert!(p.status == 403 && no_cors(&p), "{o}: {}", p.status);
        }
        let p = kit
            .request_to(NEW, "OPTIONS", "/u2/covers", &[("Origin", "http://127.0.0.1:9780"), ("Access-Control-Request-Method", "PUT")], b"")
            .await;
        assert!(p.status == 403 && no_cors(&p), "the shared address's name answers only the old address");
        let w = kit
            .request_to(
                "localhost:9781",
                "PUT",
                "/u2/covers",
                &[("Origin", "http://127.0.0.1:9780"), ("Authorization", &auth), ("If-None-Match", "*")],
                b"c",
            )
            .await;
        assert_eq!((w.status, w.header("access-control-allow-origin")), (201, Some("http://127.0.0.1:9780")));
        assert_eq!(kit.request_to("127.0.0.1", "GET", "/covers", &[], b"").await.status, 404, "the old address's stand-in has none");
    }

    // A client's writes: 6, then 429 (the old address's page reads it: CORS); an entry past the cap: 413, before its
    // body is read; each counted as refused
    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn a_clients_writes_and_the_cap_of_an_entry() {
        let kit = ug2(&["--host", "nfsu2.xyz,nfsrevived.xyz"]).await;
        let big = (nfsu2_vault::covers::CoverOptions::defaults("x".into()).max_entry + 1).to_string();
        let a = kit
            .request_to(
                NEW,
                "PUT",
                "/u2/covers",
                &[("Origin", OLD), ("Authorization", &bearer(&"A".repeat(43))), ("If-None-Match", "*"), ("Content-Length", &big)],
                b"",
            )
            .await;
        assert_eq!((a.status, a.header("access-control-allow-origin")), (413, Some(OLD)));
        for i in 0..5 {
            let t = bearer(&format!("{i:0>43}"));
            assert_eq!(
                kit.request_to(NEW, "PUT", "/u2/covers", &[("Origin", OLD), ("Authorization", &t), ("If-None-Match", "*")], b"c")
                    .await
                    .status,
                201
            );
        }
        let a = kit
            .request_to(
                NEW,
                "PUT",
                "/u2/covers",
                &[("Origin", OLD), ("Authorization", &bearer(&"Z".repeat(43))), ("If-None-Match", "*")],
                b"c",
            )
            .await;
        assert_eq!((a.status, a.header("access-control-allow-origin"), a.header("retry-after")), (429, Some(OLD), Some("3600")));
        let k = kit.app.covers.as_ref().unwrap().store.counts();
        assert_eq!((k.stored, k.refused), (5, 2));
    }
}
