//! The routes and rules: the files, the game data in blocks, the module, /players, /lan's handshake, the per-client
//! budgets, and the answers every response carries.
//!
//! There is no access control: whoever can reach the server may play, and the firewall in front of it decides who
//! that is.  What the server does instead is say nothing about the machine (no host name, addresses, paths, versions
//! or stack traces in any response; plain 4xx/5xx), keep every peer's real address to itself (nfsu2-lan), send
//! security headers on every response, keep shared caches (a CDN such as Cloudflare) from storing anything, and limit
//! what one client can make it do.  It answers only for its own names (the Host check): a page of another site that
//! points its own name at this machine (DNS rebinding) gets nothing, and neither does a scanner that connects to the
//! bare address.  The console is the operator's own: it may name paths, but it names players only by their virtual
//! address and nickname, never by their real address (--access-log is the one opt-in that records client addresses).

mod blocks;
pub mod control;
mod etag;
mod files;
mod kept;
mod lan;
mod manifest;
mod names;
mod pool;
#[cfg(test)]
mod route_tests;
mod run;
#[cfg(test)]
mod testkit;
mod turnstile;
mod url;
mod vault;
mod wasm;

use std::collections::HashMap;
use std::future::Future;
use std::net::SocketAddr;
use std::panic::AssertUnwindSafe;
use std::sync::Arc;
use std::time::Duration;

use futures_util::FutureExt;
use parking_lot::Mutex;

use nfsu2_headers::{
    Cache, HostCheck, address_key, cache_headers, client_ip, forwarded_host, is_local_or_ip, security_headers, site_key, via_https,
};
use nfsu2_http::{ClientError, Request, Response, Service, Upgraded, Version};
use nfsu2_lan::LanSwitch;
use nfsu2_online::OnlineHost;
use nfsu2_util::{Budget, Rate, js, log};

use crate::options::{Options, js_to_number};

pub use run::run;

/// What one client may hold and do (one connection's limits: nfsu2_http::Limits).  "Per client" is per client
/// address: an IPv4 address or an IPv6 /56 (nfsu2_headers::address_key); "per site" is per IPv6 /48 (site_key: one
/// site, or one free tunnel-broker allocation, which holds 256 /56s and would otherwise be 256 budgets for one client:
/// the review of the audit's F3).
pub mod limits {
    /// connections at once per client and per site, TCP peers that are not trusted proxies only: Chrome opens at
    /// most 6 per server and a WebSocket, so this leaves room for a household of players behind one address, and
    /// one client cannot take them all
    pub const CONNS_PER_ADDRESS: usize = 64;
    pub const CONNS_PER_SITE: usize = 128;
    /// requests in progress at once per client and per site, counted by the client's own address also when a proxy
    /// carries it (a proxy opens one connection here per request in progress, so without this one client behind
    /// Caddy or a tunnel could hold all of them: the audit's F3).  The page's I/O worker keeps at most 8 block
    /// requests in flight.  More gets 429.
    pub const REQUESTS_PER_CLIENT: usize = 64;
    pub const REQUESTS_PER_SITE: usize = 128;
    /// the last part of the connections (and of the requests in progress) is only for a client that holds none yet:
    /// clients that already hold some can never lock a newcomer out
    pub const FREE_SHARE: f64 = 0.25;
    /// idle keep-alive connections: longer for Cloudflare's own connections when its proxy connects here
    /// (--trusted-proxies cloudflare: it reuses an origin connection for up to 900 s and fails a request on one the
    /// origin has just closed)
    pub const KEEP_ALIVE_S: u64 = 65;
    pub const KEEP_ALIVE_CLOUDFLARE_S: u64 = 905;
    /// /lan connections at once per client, and per site
    pub const WS_PER_ADDRESS: usize = 8;
    pub const WS_PER_SITE: usize = 16;
    /// opening /lan: a burst, then this many a second, per client (twice that per site).  A page reconnects at most
    /// every 250 ms, backing off to every 5 s; a client that opens and closes connections as fast as it can gets 429
    /// (the review of the audit's F1: churning connections)
    pub const LAN_OPEN_BURST: f64 = 20.0;
    pub const LAN_OPEN_RATE: f64 = 1.0;
    /// one WebSocket message (the switch drops frames over 8 KB)
    pub const WS_MAX_PAYLOAD: usize = 64 << 10;
    pub const ARTIFACT_BYTES: usize = 64 << 20;
    /// each player holds at most 6 HTTP connections and a WebSocket, so with many players (a VPS with --max-peers
    /// 6000) the port takes 8 per player
    pub fn max_connections(max_peers: usize) -> usize {
        1024.max(max_peers * 8)
    }
}

/// What a route ended with: the answer was written to its end, the connection went first (nothing to log), or a
/// server error (the details go to this console, the client gets a plain 500).
pub enum Fail {
    Gone,
    Error(String),
}
pub type Res = Result<(), Fail>;

pub(crate) async fn send(s: &mut Response, b: &[u8]) -> Res {
    s.write(b).await.map_err(|_| Fail::Gone)
}
pub(crate) async fn finish(s: &mut Response) -> Res {
    s.end().await.map_err(|_| Fail::Gone)
}

/// A response's headers in the order they were first given (the security headers, the cache headers, then the
/// route's own): a name that is there already keeps its place and takes the new value.
pub struct Hdrs(pub Vec<(String, String)>);

impl Hdrs {
    pub fn set(&mut self, k: &str, v: impl Into<String>) {
        match self.0.iter_mut().find(|(n, _)| n == k) {
            Some(e) => e.1 = v.into(),
            None => self.0.push((k.to_string(), v.into())),
        }
    }
}

/// A share of a budget, given back when it is dropped (the answer is finished, the connection closed).
pub struct Share {
    b: Arc<Budget>,
    key: String,
    site: String,
}

impl Share {
    pub fn take(b: &Arc<Budget>, key: &str, site: &str) -> Option<Share> {
        b.take(key, site).then(|| Share { b: b.clone(), key: key.to_string(), site: site.to_string() })
    }
}

impl Drop for Share {
    fn drop(&mut self) {
        self.b.give(&self.key, &self.site);
    }
}

/// a status line's text, for the answers written on the raw connection
const STATUS: [(u16, &str); 8] = [
    (400, "Bad Request"),
    (403, "Forbidden"),
    (404, "Not Found"),
    (408, "Request Timeout"),
    (421, "Misdirected Request"),
    (429, "Too Many Requests"),
    (431, "Request Header Fields Too Large"),
    (503, "Service Unavailable"),
];

pub const ROBOTS_TXT: &str = "User-agent: *\nAllow: /\nDisallow: /data/\nDisallow: /data-manifest\nDisallow: /game.wasm\nDisallow: /game.json\nDisallow: /players\nDisallow: /lan\nDisallow: /vault\nDisallow: /names";
/// what ROBOTS_TXT keeps search engines out of, under each base
const DISALLOW: [&str; 8] = ["/data/", "/data-manifest", "/game.wasm", "/game.json", "/players", "/lan", "/vault", "/names"];

/// /robots.txt for an origin with the games under these bases ("" the root: ROBOTS_TXT)
pub fn robots_txt(bases: &[String]) -> String {
    let mut s = String::from("User-agent: *\nAllow: /");
    for b in bases {
        for d in DISALLOW {
            s.push_str("\nDisallow: ");
            s.push_str(b);
            s.push_str(d);
        }
    }
    s
}

/// the name a request asked for: an absolute URL's host, else its Host header
fn host_of<'a>(q: &'a Request, u: &'a url::Url) -> &'a str {
    if q.url.starts_with('/') { q.headers.get("host").unwrap_or("") } else { &u.host }
}

/// what a client may ask a file to be sent as
const TYPES: [(&str, &str); 11] = [
    (".html", "text/html; charset=utf-8"),
    (".js", "text/javascript; charset=utf-8"),
    (".mjs", "text/javascript; charset=utf-8"),
    (".css", "text/css; charset=utf-8"),
    (".wasm", "application/wasm"),
    (".json", "application/json"),
    (".svg", "image/svg+xml"),
    (".png", "image/png"),
    (".ico", "image/x-icon"),
    (".txt", "text/plain; charset=utf-8"),
    (".map", "application/json"),
];

pub fn type_of(ext: &str) -> Option<&'static str> {
    TYPES.iter().find(|(e, _)| *e == ext).map(|(_, t)| *t)
}

/// `/\bword\b/.test(s)`: word as a whole word (JS \w: ASCII letters, digits and "_")
pub fn has_word(s: &str, word: &str) -> bool {
    let w = |c: u8| c.is_ascii_alphanumeric() || c == b'_';
    let b = s.as_bytes();
    let mut from = 0;
    while let Some(i) = s[from..].find(word) {
        let at = from + i;
        let end = at + word.len();
        if (at == 0 || !w(b[at - 1])) && (end == b.len() || !w(b[end])) {
            return true;
        }
        from = at + 1;
    }
    false
}

/// a header that is there and not empty (an empty one counts as none)
fn present(q: &Request, name: &str) -> bool {
    q.headers.get(name).is_some_and(|v| !v.is_empty())
}

pub struct App {
    pub o: Options,
    pub host_check: HostCheck,
    /// the state folder's real path (never served: locate() and the manifest skip it)
    pub real_state: String,
    pub etag_key: Vec<u8>,
    /// the key of the /lan group ids (random each start: they stand for a client address without naming it)
    group_key: Vec<u8>,
    /// the security and cache headers, made once: [hsts][noindex][cache]
    base: Vec<Vec<(String, String)>>,
    /// the raw answers (refused upgrades, requests the engine cannot read), made once
    raw: HashMap<u16, Vec<u8>>,
    /// requests in progress per client and site (REQUESTS_PER_CLIENT, REQUESTS_PER_SITE), of max_connections in all
    pub requests: Arc<Budget>,
    pub conns: Arc<Budget>,
    pub lan_conns: Arc<Budget>,
    pub lan_opens: Rate,
    pub compressors: Arc<Budget>,
    manifest: Arc<manifest::ManifestCell>,
    wasm: wasm::WasmCell,
    real_roots: Arc<Mutex<HashMap<String, String>>>,
    pool: Arc<pool::CpuPool>,
    /// the game data's blocks at brotli's best (kept.rs), none with NFSU2_COMPRESS_THREADS=0
    kept: Option<Arc<kept::KeptBlocks>>,
    /// the /data-blocks/ answers since the 5-minute line
    sent: blocks::Sent,
    pub lan: Arc<LanSwitch>,
    pub online: Option<Arc<OnlineHost>>,
    /// free roam's hub (its `crowded()` and `in_roam()` for /players), none with --roam-hub off
    pub roam: Option<Arc<nfsu2_roam::RoamHub>>,
    /// keyphrase saves (/vault), none with --vault-gb 0
    pub vault: Option<vault::VaultRoute>,
    /// claimed names (/names), none without keyphrase saves
    pub names: Option<names::NamesRoute>,
    /// the operator's bans (the control socket: control.rs), checked at /lan
    pub bans: Arc<nfsu2_bans::Bans>,
    /// SO_SNDBUF for /lan connections (NFSU2_LAN_SNDBUF, a measurement knob; 0: the kernel's own, which Linux grows
    /// to megabytes, ahead of the switch's own queue rules)
    lan_sndbuf: usize,
}

fn cache_index(c: Cache) -> usize {
    match c {
        Cache::Store => 0,
        Cache::Revalidate => 1,
        Cache::Immutable => 2,
    }
}

impl App {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        o: Options,
        real_state: String,
        etag_key: Vec<u8>,
        lan: Arc<LanSwitch>,
        online: Option<Arc<OnlineHost>>,
        roam: Option<Arc<nfsu2_roam::RoamHub>>,
        vault: Option<Arc<nfsu2_vault::Vault>>,
        names: Option<names::NamesRoute>,
        bans: Arc<nfsu2_bans::Bans>,
        pool_threads: usize,
        lan_sndbuf: usize,
        compress_threads: usize,
    ) -> App {
        let host_check = HostCheck::new(&o.hosts);
        let real_roots: Arc<Mutex<HashMap<String, String>>> = Arc::default();
        let manifest: Arc<manifest::ManifestCell> = Arc::default();
        // the kept blocks: in the state folder, swept again for each new manifest
        let kept = (compress_threads > 0)
            .then(|| match kept::KeptBlocks::open(std::path::Path::new(&real_state).join("blocks"), compress_threads) {
                Ok(k) => Some(k),
                Err(e) => {
                    log::error(&format!("kept blocks: none ({e})"));
                    None
                }
            })
            .flatten();
        if let Some(k) = &kept {
            let (k, game, data, roots, state) = (k.clone(), o.game, o.data.clone(), real_roots.clone(), real_state.clone());
            manifest.on_new(Arc::new(move |m: &manifest::Manifest| {
                let (data, roots, state) = (data.clone(), roots.clone(), state.clone());
                k.sweep(m.etag(), m.files.clone(), move |rel| {
                    let file = files::resolve_game(game, &data, &format!("/{rel}"))?;
                    let f = files::locate_sync(&roots, &state, &data, &file)?;
                    Some((f.real, f.md))
                });
            }));
        }
        let mut base = Vec::new();
        for hsts in [false, true] {
            for noindex in [false, true] {
                for cache in [Cache::Store, Cache::Revalidate, Cache::Immutable] {
                    let mut h: Vec<(String, String)> =
                        security_headers(hsts, noindex).into_iter().map(|(k, v)| (k.to_string(), v)).collect();
                    // Cloudflare's check where names are claimed (turnstile.rs): its script and frame
                    if names.as_ref().is_some_and(|n| n.turnstile.is_some()) {
                        for (k, v) in h.iter_mut().filter(|(k, _)| k == "content-security-policy") {
                            let _ = k;
                            *v = nfsu2_headers::CSP_TURNSTILE.to_string();
                        }
                    }
                    for (k, v) in cache_headers(cache) {
                        h.push((k.to_string(), v.to_string()));
                    }
                    base.push(h);
                }
            }
        }
        let mut raw = HashMap::new();
        for (code, text) in STATUS {
            // the same headers as every answer, nothing else
            let mut s = format!("HTTP/1.1 {code} {text}\r\n");
            let mut h: Vec<(String, String)> = security_headers(false, true).into_iter().map(|(k, v)| (k.to_string(), v)).collect();
            for (k, v) in cache_headers(Cache::Store) {
                h.push((k.to_string(), v.to_string()));
            }
            h.push(("content-length".into(), "0".into()));
            h.push(("connection".into(), "close".into()));
            s.push_str(&h.iter().map(|(k, v)| format!("{k}: {v}")).collect::<Vec<_>>().join("\r\n"));
            s.push_str("\r\n\r\n");
            raw.insert(code, s.into_bytes());
        }
        let max_conns = limits::max_connections(o.max_peers);
        App {
            host_check,
            real_state,
            etag_key,
            group_key: nfsu2_util::rand::bytes(32),
            base,
            raw,
            requests: Arc::new(Budget::new(limits::REQUESTS_PER_CLIENT, limits::REQUESTS_PER_SITE, max_conns, limits::FREE_SHARE)),
            conns: Arc::new(Budget::new(limits::CONNS_PER_ADDRESS, limits::CONNS_PER_SITE, max_conns, limits::FREE_SHARE)),
            lan_conns: Arc::new(Budget::new(limits::WS_PER_ADDRESS, limits::WS_PER_SITE, usize::MAX, 0.0)),
            lan_opens: Rate::new(limits::LAN_OPEN_BURST, limits::LAN_OPEN_RATE),
            compressors: Arc::new(Budget::new(
                blocks::COMPRESSORS_PER_CLIENT,
                blocks::COMPRESSORS_PER_SITE,
                blocks::COMPRESSORS_TOTAL,
                0.0,
            )),
            manifest,
            wasm: wasm::WasmCell::default(),
            real_roots,
            pool: Arc::new(pool::CpuPool::new(pool_threads, "nfsu2-blocks")),
            kept,
            sent: blocks::Sent::default(),
            lan,
            online,
            roam,
            vault: vault.map(vault::VaultRoute::new),
            names,
            bans,
            lan_sndbuf,
            o,
        }
    }

    /// HSTS only over a certificate browsers check (this server's own or a proxy's that says https), never for
    /// localhost or an IP literal
    fn hsts_for(&self, q: &Request) -> bool {
        self.o.hsts_ok && via_https(q, &self.o.trust) && {
            let fwd = forwarded_host(q, &self.o.trust);
            !is_local_or_ip(fwd.as_deref().or(q.headers.get("host")))
        }
    }

    /// `headers(q, {}, cache, noindex)`: the security headers, then the cache headers; the caller sets the rest
    pub fn headers(&self, q: &Request, cache: Cache, noindex: bool) -> Hdrs {
        let i = (self.hsts_for(q) as usize) * 6 + (noindex as usize) * 3 + cache_index(cache);
        Hdrs(self.base[i].clone())
    }

    /// a short text answer: the code, the text (a newline after it), headers besides the usual ones, the cache rule,
    /// and whether search engines are told to keep out
    #[allow(clippy::too_many_arguments)]
    pub async fn plain(
        &self,
        q: &Request,
        s: &mut Response,
        code: u16,
        text: &str,
        extra: &[(&str, &str)],
        cache: Cache,
        noindex: bool,
    ) -> Res {
        let body = format!("{text}\n");
        let mut h = self.headers(q, cache, noindex);
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

    pub async fn not_found(&self, q: &Request, s: &mut Response) -> Res {
        self.plain(q, s, 404, "Not found", &[], Cache::Store, true).await
    }

    async fn bad_request(&self, q: &Request, s: &mut Response) -> Res {
        self.plain(q, s, 400, "Bad request", &[], Cache::Store, true).await
    }

    /// the answer for code written on the raw connection (refused upgrades, requests the engine cannot read): the
    /// same headers as every answer, nothing else (a code it has none for: the 400)
    pub fn raw_response(&self, code: u16) -> Vec<u8> {
        self.raw.get(&code).cloned().unwrap_or_else(|| self.raw[&400].clone())
    }

    /// where the routes are on the name the request asked for (PLAN D-38): "" (the root) or a base path ("/u2": its
    /// routes are /u2/, /u2/game.json, /u2/lan, ...)
    fn base_of(&self, q: &Request, u: &url::Url) -> &str {
        self.o.bases.of(host_of(q, u))
    }

    /// /robots.txt at the root of a name where this game's routes are under a base (the origin it shares with the
    /// other game, whose routes it names too: crawlers read only the root's)
    fn shared_robots(&self, host: &str) -> String {
        let mut bases = vec![self.o.bases.of(host).to_string()];
        for g in crate::game::GAMES.into_iter().filter(|g| g.id != self.o.game.id) {
            let b = crate::game::Bases::of_game(g).of(host).to_string();
            if !b.is_empty() && !bases.contains(&b) {
                bases.push(b);
            }
        }
        robots_txt(&bases)
    }

    /// the request's Host (and an absolute-form URL's host) is one of this server's names
    fn own_host(&self, q: &Request) -> bool {
        if !self.host_check.check(q.headers.get("host")) {
            return false;
        }
        if !q.url.starts_with('/') {
            match url::parse_absolute(&q.url) {
                Some(u) if self.host_check.check(Some(&u.host)) => {}
                _ => return false,
            }
        }
        true
    }

    /// A request from one of this server's own pages, or from no page at all (tools send no Origin): its Origin, when
    /// it has one, is the host it asked for (or the one a trusted proxy says it asked for) and one of this server's
    /// names.  A page of another site must not reach the LAN, or a player's saves, through a player's browser.
    fn own_page(&self, q: &Request) -> bool {
        let Some(o) = q.headers.get("origin").filter(|o| !o.is_empty()) else { return true };
        let Some(origin) = url::parse_absolute(o).map(|x| x.host.to_lowercase()) else { return false };
        let host = q.headers.get("host").unwrap_or("").to_lowercase();
        let fwd = forwarded_host(q, &self.o.trust);
        let ours = (!host.is_empty() && host == origin) || fwd.as_deref().is_some_and(|f| !f.is_empty() && f == origin);
        ours && self.host_check.check(Some(&origin))
    }

    /// the client's address, and the keys its budgets count by
    fn client(&self, q: &Request) -> (String, String, String) {
        let ip = client_ip(q, &self.o.trust, &self.o.real_ip);
        let key = address_key(&ip);
        let site = site_key(&ip);
        (ip, key, site)
    }

    /// a group id: the client's (or site's) budget key under a keyed hash, so the switch can count by it without
    /// ever holding the address
    fn group_id(&self, key: &str) -> String {
        use hmac::{Hmac, Mac};
        let mut mac = <Hmac<sha2::Sha256> as Mac>::new_from_slice(&self.group_key).expect("HMAC takes any key");
        mac.update(key.as_bytes());
        js::base64url(&mac.finalize().into_bytes())[..22].to_string()
    }

    /// One request: the requests budget, the Cloudflare keep-alive rule, the access log, and the handler with its 500
    async fn on_req(&self, mut q: Request, s: &mut Response) {
        // with Cloudflare's proxy in front, the long keep-alive is for the proxies' connections: any other peer gets
        // the usual one, and is told so (keep_alive() gives the engine the time itself)
        if self.o.trust.cloudflare && !self.o.trust.trusts(&q.conn.peer) {
            s.set_header("Keep-Alive", &format!("timeout={}", limits::KEEP_ALIVE_S));
        }
        let (ip, key, site) = self.client(&q);
        let Some(_counted) = Share::take(&self.requests, &key, &site) else {
            log::limited(
                "http",
                "refused requests",
                &key,
                &format!("[http] refused a request: {} in progress already from that client", self.requests.held(&key)),
                2,
            );
            let _ = self.plain(&q, s, 429, "Too many requests", &[("connection", "close"), ("retry-after", "1")], Cache::Store, true).await;
            return;
        };
        let t0 = nfsu2_util::now_ms();
        let r = AssertUnwindSafe(self.handler(&mut q, s)).catch_unwind().await;
        let error = match r {
            Ok(Ok(())) => None,
            Ok(Err(Fail::Gone)) => return,
            Ok(Err(Fail::Error(e))) => Some(e),
            Err(p) => Some(
                p.downcast_ref::<&str>()
                    .map(|s| s.to_string())
                    .or_else(|| p.downcast_ref::<String>().cloned())
                    .unwrap_or_else(|| "a panic".into()),
            ),
        };
        if let Some(e) = error {
            // the details stay on this console; the client gets a plain 500
            log::error(&format!("[error] {} {}: {e}", js::printable(&q.method, 8), js::printable(&q.url, 96)));
            if s.head_sent() {
                s.destroy();
                return;
            }
            if self.plain(&q, s, 500, "Server error", &[], Cache::Store, true).await.is_err() {
                return;
            }
        }
        if self.o.access_log {
            // the opt-in that names client addresses; a page loads its game data in thousands of requests: plenty
            // for one client, not a flood.  (The method and the URL as every console line has them: one line,
            // printable, short; a client chooses what is in a URL.)
            let line = format!(
                "[http] {} {} {} {} {} ms",
                if ip.is_empty() { "?" } else { &ip },
                js::printable(&q.method, 8),
                js::printable(&q.url, 160),
                s.status(),
                nfsu2_util::now_ms() - t0
            );
            log::limited("http", "requests", &key, &line, 3000);
        }
    }

    /// the router
    async fn handler(&self, q: &mut Request, s: &mut Response) -> Res {
        const CLOSE: &[(&str, &str)] = &[("connection", "close")];
        // HTTP/1.1 needs a Host (checked here, not by the engine, so the answer has our headers)
        if !present(q, "host") && q.version != Version::Http10 {
            return self.plain(q, s, 400, "Bad request", CLOSE, Cache::Store, true).await;
        }
        // only this server's own names (DNS rebinding, scanners on the bare address): before anything else
        if !self.own_host(q) {
            return self.plain(q, s, 421, "Misdirected request", CLOSE, Cache::Store, true).await;
        }
        // a path ("/x", origin form) is read as a path, never as "//host/path"; an absolute URL as itself.  No encoded
        // separator and, once decoded, no empty segment: every route compares the path it sees, and a later
        // normalisation that merged "//" or turned %2f into "/" would hand a file a different path than the one
        // checked (the phase-2 deploy review's /%2ftest/... finding)
        let Some(u) = url::parse_target(&q.url) else { return self.bad_request(q, s).await };
        let lower = u.pathname.to_ascii_lowercase();
        if lower.contains("%2f") || lower.contains("%5c") {
            return self.bad_request(q, s).await;
        }
        let Some(p) = url::decode_uri_component(&u.pathname) else { return self.bad_request(q, s).await };
        if p.chars().any(|c| c <= '\u{1f}' || c == '\u{7f}' || c == '\\') || p.contains("//") {
            return self.bad_request(q, s).await;
        }
        // the routes under the base of the name asked for (D-38): "/u2/x" is the route "/x" there; "/u2" itself and
        // the rest of that origin are no route of this game's (below)
        let base = self.base_of(q, &u);
        let rest = if base.is_empty() { None } else { p.strip_prefix(base).filter(|r| r.starts_with('/')).map(str::to_string) };
        let under = base.is_empty() || rest.is_some();
        let p = rest.unwrap_or(p);
        if under && q.method == "PUT" && self.o.dev && !self.o.artifacts.is_empty() && p.starts_with("/artifacts/") {
            return self.put_artifact(q, s, &p[11..]).await;
        }
        // keyphrase saves and claimed names: their own methods and bodies (vault.rs, names.rs)
        if under && p == "/vault" {
            return self.vault(q, s, &u).await;
        }
        if under && p == "/names" {
            return self.names(q, s, &u).await;
        }
        if q.method != "GET" && q.method != "HEAD" {
            return self
                .plain(q, s, 405, "Method not allowed", &[("allow", "GET, HEAD"), ("connection", "close")], Cache::Store, true)
                .await;
        }
        // nothing here takes a request body
        if js_to_number(q.headers.get("content-length").unwrap_or("0")) > 0.0 || present(q, "transfer-encoding") {
            return self.plain(q, s, 413, "Too large", CLOSE, Cache::Store, true).await;
        }
        if !under {
            // the base without its "/": to the page at its folder, the query kept (an edge redirects other spellings
            // there first)
            if p == base {
                let to = match &u.query {
                    Some(query) => format!("{base}/?{query}"),
                    None => format!("{base}/"),
                };
                return self.plain(q, s, 308, "Permanent redirect", &[("location", to.as_str())], Cache::Store, true).await;
            }
            // that origin's root files, which belong to no game's base (UG2's server answers them: game.rs)
            if self.o.game.root_files && p == "/robots.txt" {
                let text = self.shared_robots(host_of(q, &u));
                return self.plain(q, s, 200, &text, &[], Cache::Revalidate, false).await;
            }
            return self.not_found(q, s).await;
        }
        if p == "/data-manifest" {
            return self.send_manifest(q, s, &u, base).await;
        }
        // the game's files are bytes for the game, whatever their names say
        if let Some(rest) = p.strip_prefix("/data")
            && rest.starts_with('/')
        {
            let file = files::resolve_game(self.o.game, &self.o.data, rest);
            return self
                .send_file(q, s, &self.o.data, file, files::FileOpts { type_: Some("application/octet-stream"), ..Default::default() })
                .await;
        }
        if let Some(rest) = p.strip_prefix("/data-blocks")
            && rest.starts_with('/')
        {
            // (the manifest's addresses since blocks are kept: the same files)
            let rest = rest.strip_prefix(blocks::V2).filter(|r| r.starts_with('/')).unwrap_or(rest);
            let file = files::resolve_game(self.o.game, &self.o.data, rest);
            return self.send_blocks(q, s, file, &u).await;
        }
        if p == "/game.wasm" {
            return self.send_wasm(q, s, &u).await;
        }
        if p == "/game.json" {
            return self.send_game_json(q, s).await;
        }
        if p == "/players" {
            return self.send_players(q, s, &u).await;
        }
        // (crawlers read it at an origin's root only: under a base it is no route)
        if p == "/robots.txt" {
            if !base.is_empty() {
                return self.not_found(q, s).await;
            }
            return self.plain(q, s, 200, ROBOTS_TXT, &[], Cache::Revalidate, false).await;
        }
        // test aids (--dev only): the repo's test scripts, and the test pages and source maps from their own folder
        // (web/dist-dev), which the server never looks in without --dev
        if self.o.dev && p.starts_with("/tests/") {
            let tests = self.o.tests.clone();
            let file = files::resolve_inside(&tests, &p[6..]);
            return self.send_file(q, s, &tests, file, files::FileOpts::default()).await;
        }
        // a second guard, for a web/dist that an older build left test pages or maps in: their names, compared the
        // way a case-insensitive disk does (NFKC folds "ſ" to "s", "K" to "k")
        if !self.o.dev && files::dev_only(&p) {
            return self.not_found(q, s).await;
        }
        let rel = if p.ends_with('/') { format!("{p}index.html") } else { p.clone() };
        let mut root = self.o.dist.clone();
        let mut found = self.locate(&root, files::resolve_inside(&root, &rel)).await;
        if found.is_none() && self.o.dev {
            root = self.o.dev_dist.clone();
            found = self.locate(&root, files::resolve_inside(&root, &rel)).await;
        }
        let indexable = root == self.o.dist && rel == "/index.html";
        let cache = if rel.ends_with(".html") { Cache::Store } else { Cache::Revalidate };
        self.send_file(q, s, &root, None, files::FileOpts { found, cache, noindex: !indexable, ..Default::default() }).await
    }

    /// /data-manifest: built at the start, again on ?refresh at most every 10 s; the body for the pages under `base`
    async fn send_manifest(&self, q: &Request, s: &mut Response, u: &url::Url, base: &str) -> Res {
        let fail = |e: String| Fail::Error(format!("the manifest: {e}"));
        let mut m = match self.current_manifest() {
            Some(m) if !(u.has_param("refresh") && nfsu2_util::now_ms() - m.at > 10_000) => m,
            _ => self.refresh_manifest().await.map_err(fail)?,
        };
        // a base the walk made no body for: a name the server took on since (a new certificate's), walked again now
        // that it is one (twice at most: a walk already under way when the name came has none either)
        for _ in 0..2 {
            if m.has(base) {
                break;
            }
            m = self.refresh_manifest().await.map_err(fail)?;
        }
        let m = m.under(base);
        let gz = has_word(q.headers.get("accept-encoding").unwrap_or(""), "gzip");
        let mut h = self.headers(q, Cache::Revalidate, true);
        h.set("content-type", "application/json");
        h.set("etag", m.etag.clone());
        h.set("vary", "accept-encoding");
        if nfsu2_headers::etag_matches(q.headers.get("if-none-match"), &m.etag) {
            s.head(304, h.0);
            return finish(s).await;
        }
        let body = if gz { &m.gz } else { &m.body };
        h.set("content-length", body.len().to_string());
        if gz {
            h.set("content-encoding", "gzip");
        }
        s.head(200, h.0);
        if q.method != "HEAD" {
            send(s, body).await?;
        }
        finish(s).await
    }

    /// the manifest walked again: one walk at a time; a request while one is under way takes its result.  A body for
    /// each base of the names the server answers now (none for the shared site's /u2 where that name gets 421)
    pub async fn refresh_manifest(&self) -> Result<Arc<manifest::Manifest>, String> {
        let bases = self.o.bases.all(|n| self.host_check.check(Some(n)));
        self.manifest.refresh(self.o.game, &bases, &self.o.data, &self.real_state, &self.etag_key).await
    }

    /// the manifest made last
    pub fn current_manifest(&self) -> Option<Arc<manifest::Manifest>> {
        self.manifest.current()
    }

    pub fn manifest_count(&self) -> usize {
        self.current_manifest().map_or(0, |m| m.count)
    }

    /// /players: how many play on this server now (the start page's Multiplayer settings): three counts, whether it
    /// is full and whether free roam is busy, nothing else, no names, addresses or times; online: in the game's Online
    /// mode (logged in, or racing an online race), null with --online off; roam: driving in free roam, or paused
    /// there, as of the hub's last tick (nfsu2-roam), null with --roam-hub off; full: a game that connects now waits in
    /// line (a page of before: gets "LAN full"); queue: the pages waiting in line (nfsu2-lan queue.rs); roamBusy: the
    /// hub gives fewer than askCap links a player, so the page does not ask the free-roam question now (false with
    /// --roam-hub off).  ?room=<hash> (roam_core's rc_room_hash of a Room setting, decimal,
    /// not 0) adds room: how many of roam are in that room (with --roam-hub on).  ?crews=<hash>,<hash>,... (the
    /// rc_room_hash of each of the Crews setting's words, decimal; the first 4 that are numbers and not 0, the rest
    /// left out) adds crews: how many of roam are in each of those crews, in any room, in that order (with --roam-hub
    /// on, and one of them at least); both as of the hub's last tick, from its counts (nothing waits for its thread)
    async fn send_players(&self, q: &Request, s: &mut Response, u: &url::Url) -> Res {
        let info = match &self.online {
            Some(h) => {
                let rx = nfsu2_util::Host::info(h.as_ref());
                tokio::time::timeout(Duration::from_secs(2), rx).await.ok().and_then(Result::ok)
            }
            None => None,
        };
        let players = self.lan.peer_count();
        let online = match info {
            Some(o) => {
                let n = |k: &str| o.get(k).and_then(serde_json::Value::as_f64).unwrap_or(f64::NAN);
                let x = n("personas") + n("racing");
                // Math.min: NaN when either is
                nfsu2_util::json::num(if x.is_nan() { f64::NAN } else { (players as f64).min(x) })
            }
            None => serde_json::Value::Null,
        };
        // (Math.min as online: the hub's last tick and the switch now, a player who just left may still be in it)
        let roam = self.roam.as_ref().map_or(serde_json::Value::Null, |r| serde_json::Value::from(r.in_roam().min(players)));
        let mut m = serde_json::Map::new();
        m.insert("players".into(), serde_json::Value::from(players));
        m.insert("online".into(), online);
        m.insert("roam".into(), roam);
        m.insert("full".into(), serde_json::Value::Bool(self.lan.full()));
        m.insert("queue".into(), serde_json::Value::from(self.lan.waiting()));
        m.insert("roamBusy".into(), serde_json::Value::Bool(self.roam.as_ref().is_some_and(|r| r.crowded())));
        let room = u.param("room").and_then(|r| r.parse::<u32>().ok()).filter(|&r| r != 0);
        if let (Some(room), Some(r)) = (room, &self.roam) {
            m.insert("room".into(), serde_json::Value::from(r.in_room(room).min(players)));
        }
        let crews: Vec<u32> = u.param("crews").map_or(Vec::new(), |c| {
            c.split(',').filter_map(|h| h.parse::<u32>().ok()).filter(|&h| h != 0).take(nfsu2_roam::CREWS).collect()
        });
        if let Some(r) = self.roam.as_ref().filter(|_| !crews.is_empty()) {
            let n = r.in_crews(&crews).into_iter().map(|n| serde_json::Value::from(n.min(players))).collect();
            m.insert("crews".into(), serde_json::Value::Array(n));
        }
        let body = nfsu2_util::json::stringify(&serde_json::Value::Object(m));
        let mut h = self.headers(q, Cache::Store, true);
        h.set("content-type", "application/json");
        h.set("content-length", body.len().to_string());
        s.head(200, h.0);
        if q.method != "HEAD" {
            send(s, body.as_bytes()).await?;
        }
        finish(s).await
    }

    /// a test artifact (frame dump) from a page on this machine (--dev --artifacts only)
    async fn put_artifact(&self, q: &mut Request, s: &mut Response, name: &str) -> Res {
        let peer = nfsu2_headers::bare_ip(&q.conn.peer);
        let local = (peer == "::1" || is_loopback4(&peer)) && !present(q, "x-forwarded-for") && !present(q, "cf-connecting-ip");
        if !local || !artifact_name(name) {
            return self.plain(q, s, 403, "Forbidden", &[], Cache::Store, true).await;
        }
        if js_to_number(q.headers.get("content-length").unwrap_or("0")) > limits::ARTIFACT_BYTES as f64 {
            return self.plain(q, s, 413, "Too large", &[("connection", "close")], Cache::Store, true).await;
        }
        let body = match q.read_body(limits::ARTIFACT_BYTES).await {
            Ok(b) => b,
            Err(nfsu2_http::BodyError::TooLarge) => {
                return self.plain(q, s, 413, "Too large", &[("connection", "close")], Cache::Store, true).await;
            }
            Err(nfsu2_http::BodyError::Broken) => {
                s.destroy();
                return Err(Fail::Gone);
            }
        };
        let path = crate::paths::join(&self.o.artifacts, name);
        tokio::fs::write(&path, body).await.map_err(|e| Fail::Error(format!("{path}: {e}")))?;
        let mut h = self.headers(q, Cache::Store, true);
        h.set("content-length", "0");
        s.head(201, h.0);
        finish(s).await
    }
}

/// `/^127\.\d+\.\d+\.\d+$/`
fn is_loopback4(ip: &str) -> bool {
    let parts: Vec<&str> = ip.split('.').collect();
    parts.len() == 4 && parts[0] == "127" && parts[1..].iter().all(|p| !p.is_empty() && p.bytes().all(|c| c.is_ascii_digit()))
}

/// `/^[A-Za-z0-9_-][A-Za-z0-9_.-]{0,127}\.png$/`
fn artifact_name(name: &str) -> bool {
    let ok = |c: u8, dot: bool| c.is_ascii_alphanumeric() || c == b'_' || c == b'-' || (dot && c == b'.');
    let Some(stem) = name.strip_suffix(".png") else { return false };
    let b = stem.as_bytes();
    !b.is_empty() && b.len() <= 128 && ok(b[0], false) && b[1..].iter().all(|&c| ok(c, true))
}

impl Service for App {
    /// connections at once per client and site, for TCP peers that are not trusted proxies (a proxy carries many
    /// clients); counted from the first byte, before any TLS, so stalled handshakes count too
    fn admit(&self, peer: &SocketAddr) -> Option<Box<dyn Send + Sync>> {
        let ip = nfsu2_headers::bare_ip(&peer.ip().to_string());
        if ip.is_empty() {
            return None;
        }
        if self.o.trust.trusts(&ip) {
            return Some(Box::new(()));
        }
        let key = address_key(&ip);
        let site = site_key(&ip);
        Share::take(&self.conns, &key, &site).map(|s| Box::new(s) as Box<dyn Send + Sync>)
    }

    /// Cloudflare's addresses (--trusted-proxies cloudflare) show Cloudflare's origin-pull certificate: any of its
    /// customers connects from them too (nfsu2_headers::ProxyTrust)
    fn client_certificate(&self, peer: &SocketAddr) -> bool {
        self.o.trust.must_prove(&peer.ip().to_string())
    }

    fn keep_alive(&self, peer: &SocketAddr) -> Duration {
        if !self.o.trust.cloudflare {
            return Duration::from_secs(limits::KEEP_ALIVE_S);
        }
        if self.o.trust.trusts(&peer.ip().to_string()) {
            return Duration::from_secs(limits::KEEP_ALIVE_CLOUDFLARE_S);
        }
        // the usual time for a peer that is not Cloudflare's: 65 s from the end of the answer.  The engine adds a
        // second to every keep-alive time (as Node does: nfsu2_http's KEEP_ALIVE_BUFFER), so a second less here: the
        // Node server set the socket's timeout to 65 s once an answer was finished, which replaced Node's own
        // keep-alive timer, its extra second and all.  (on_req says timeout=65 in the Keep-Alive header itself.)
        Duration::from_secs(limits::KEEP_ALIVE_S - 1)
    }

    fn request<'a>(&'a self, req: Request, res: &'a mut Response) -> impl Future<Output = ()> + Send + 'a {
        self.on_req(req, res)
    }

    /// Expect: anything but 100-continue
    async fn expectation<'a>(&'a self, req: Request, res: &'a mut Response) {
        let _ = self.plain(&req, res, 417, "Expectation failed", &[("connection", "close")], Cache::Store, true).await;
    }

    fn upgrade(&self, req: Request, up: Upgraded) -> impl Future<Output = ()> + Send {
        self.on_upgrade(req, up)
    }

    /// A request the engine could not read (a parse error, a head over max_header_size, a header or request that
    /// took too long) gets a short answer with this server's headers, then the connection goes.  A TLS handshake that
    /// failed or took too long has no HTTP to answer with: the engine closes the connection without a byte.
    fn client_error(&self, e: ClientError) -> Option<Vec<u8>> {
        Some(self.raw_response(match e {
            ClientError::Parse => 400,
            ClientError::HeaderOverflow => 431,
            ClientError::Timeout => 408,
        }))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn words_as_js_regexes_find_them() {
        assert!(has_word("gzip, deflate, br", "br"));
        assert!(has_word("br", "br"));
        assert!(has_word("gzip;q=1", "gzip"));
        assert!(!has_word("brotli", "br"));
        assert!(!has_word("x-gzip2", "gzip"));
        assert!(!has_word("BR", "br"));
        assert!(has_word("x-gzip", "gzip"));
        assert!(has_word("abr, br", "br"));
    }

    #[test]
    fn artifact_names() {
        for ok in ["x.png", "frame_001.png", "-a.png", "a.png.png", &format!("a{}.png", "b".repeat(127))] {
            assert!(artifact_name(ok), "{ok}");
        }
        for bad in [".png", "..png", ".x.png", "a/b.png", "a.PNG", "x.png ", &format!("a{}.png", "b".repeat(128)), "é.png"] {
            assert!(!artifact_name(bad), "{bad}");
        }
        assert!(is_loopback4("127.0.0.1") && is_loopback4("127.10.200.3"));
        assert!(!is_loopback4("127.0.0") && !is_loopback4("128.0.0.1") && !is_loopback4("127.0.0.x"));
    }

    // robots.txt at a root of one game's (as ever), and of the shared site's two bases
    #[test]
    fn robots_for_each_site() {
        assert_eq!(robots_txt(&[String::new()]), ROBOTS_TXT);
        let two = robots_txt(&["/u2".into(), "/mw".into()]);
        assert_eq!(two.lines().count(), 2 + 2 * DISALLOW.len());
        assert!(
            two.starts_with("User-agent: *\nAllow: /\nDisallow: /u2/data/\n")
                && two.ends_with("\nDisallow: /mw/vault\nDisallow: /mw/names")
        );
    }

    #[test]
    fn headers_keep_their_first_place() {
        let mut h = Hdrs(vec![("a".into(), "1".into()), ("cdn-cache-control".into(), "no-store".into())]);
        h.set("cdn-cache-control", "public");
        h.set("b", "2");
        assert_eq!(h.0, [("a", "1"), ("cdn-cache-control", "public"), ("b", "2")].map(|(k, v)| (k.to_string(), v.to_string())));
    }
}
