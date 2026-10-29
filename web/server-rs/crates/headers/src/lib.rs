//! What every response of the server carries, and who the client is.
//!
//! - [`security_headers`]: cross-origin isolation (COOP/COEP/CORP, which the game needs for SharedArrayBuffer), a
//!   Content-Security-Policy, nosniff, no referrer, no framing, a Permissions-Policy with only what the game uses,
//!   noindex, and HSTS when the server asks for it
//! - [`pick_encoding`]: the Content-Encoding for an Accept-Encoding header
//! - [`cache_headers`]: no shared cache (a reverse proxy, Cloudflare's edge) ever keeps a response; the browser
//!   keeps only what it revalidates (the game module, the page's scripts)
//! - [`ProxyTrust`]: which TCP peers are proxies whose CF-Connecting-IP / X-Forwarded-* headers are believed (only
//!   those)
//! - [`client_ip`], [`via_https`], [`forwarded_host`], [`address_key`], [`site_key`]
//! - [`HostCheck`]: the names the server answers for (Host, Origin)
//!
//! There is no access control here on purpose: whoever can reach the server may play, and the firewall in front of
//! it decides who that is.  The client address is used for the server's own console and its per-address limits
//! only.  Pure functions over strings; a request is seen through [`Req`].  Addresses are read as Node reads them
//! (`net.isIP`, its BlockList: the rules of the Node server this replaced), host names as the WHATWG URL parser
//! writes them.

use std::borrow::Cow;
use std::collections::HashSet;
use std::path::Path;

use base64::Engine;
use nfsu2_util::js;
use parking_lot::RwLock;
use sha2::{Digest, Sha256};

mod ip;
mod visitor;

use ip::Ip;

/// The parts of a request these functions read (the HTTP crate's `Request` implements it).
pub trait Req {
    /// the TCP peer's address as Node gives `socket.remoteAddress`: "127.0.0.1", "::1", or "::ffff:1.2.3.4" for an
    /// IPv4 client of a dual-stack ("::") listener
    fn peer(&self) -> &str;
    /// a header by its lower-case name, as Node's `q.headers[name]` has it (duplicates joined as Node joins them)
    fn header(&self, name: &str) -> Option<&str>;
    /// the connection is TLS (Node's `socket.encrypted`)
    fn encrypted(&self) -> bool;
}

// ---- Content-Security-Policy ---------------------------------------------------

// The policy's directives, before and after the hashes a page may add to script-src and style-src.
macro_rules! csp_head {
    () => {
        "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'"
    };
}
macro_rules! csp_mid {
    () => {
        "; worker-src 'self'; connect-src 'self'; img-src 'self' data: blob:; style-src 'self'"
    };
}
macro_rules! csp_tail {
    () => {
        "; font-src 'self'; media-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; \
         frame-src 'none'; frame-ancestors 'none'"
    };
}

/// The Content-Security-Policy without hashes (every response but a page with inline code of its own).
pub const CSP: &str = concat!(csp_head!(), csp_mid!(), csp_tail!());

/// [`CSP`] with Cloudflare Turnstile's script and frame (challenges.cloudflare.com), for a server whose claimed names
/// ask for its check (the server's turnstile.rs)
pub const CSP_TURNSTILE: &str = concat!(
    csp_head!(),
    " https://challenges.cloudflare.com",
    csp_mid!(),
    "; font-src 'self'; media-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; \
     frame-src https://challenges.cloudflare.com; frame-ancestors 'none'"
);

/// The policy with a page's own hashes in script-src and style-src.  Everything comes from this origin.
/// WebAssembly needs 'wasm-unsafe-eval' (compiling the module, in the page and in its workers, whose scripts carry
/// this header too).  'self' covers the module workers, the audio worklet and the /lan WebSocket (CSP 3: ws: and
/// wss: to this host).  Images: the favicon is a data: URL; screenshots in the Saves panel may be blob: URLs.  No
/// inline script or style, except where a page lists the hashes of its own.
pub fn content_security_policy(script_hashes: &[&str], style_hashes: &[&str]) -> String {
    let mut s = String::with_capacity(CSP.len() + 64 * (script_hashes.len() + style_hashes.len()));
    let hashes = |s: &mut String, list: &[&str]| {
        for h in list {
            s.push_str(" '");
            s.push_str(h);
            s.push('\'');
        }
    };
    s.push_str(csp_head!());
    hashes(&mut s, script_hashes);
    s.push_str(csp_mid!());
    hashes(&mut s, style_hashes);
    s.push_str(csp_tail!());
    s
}

/// "sha256-<base64>" of the text: a CSP source for an inline script or style
pub fn sha256_source(text: &str) -> String {
    let digest = Sha256::digest(text.as_bytes());
    format!("sha256-{}", base64::engine::general_purpose::STANDARD.encode(digest))
}

/// The Permissions-Policy.  Only what the game uses: game controllers, full screen, the keyboard layout
/// (key names in the settings; Keyboard Lock itself has no policy and needs full screen), sound without a
/// click first, the screen kept on while a game runs (a controller's buttons do not keep every system's
/// screen awake), and the motion sensor for the touch controls' tilt steering (the device orientation events
/// need accelerometer and gyroscope).  The rest is switched off for this page and anything it could embed.
pub const PERMISSIONS_POLICY: &str = concat!(
    "gamepad=(self), fullscreen=(self), keyboard-map=(self), autoplay=(self), cross-origin-isolated=(self), ",
    "screen-wake-lock=(self), accelerometer=(self), gyroscope=(self), ",
    "bluetooth=(), browsing-topics=(), camera=(), clipboard-read=(), display-capture=(), ",
    "encrypted-media=(), geolocation=(), hid=(), idle-detection=(), local-fonts=(), ",
    "magnetometer=(), microphone=(), midi=(), payment=(), publickey-credentials-get=(), ",
    "serial=(), usb=(), window-management=(), xr-spatial-tracking=()",
);

/// [`PERMISSIONS_POLICY`]
pub fn permissions_policy() -> &'static str {
    PERMISSIONS_POLICY
}

/// What makes the page cross-origin isolated (SharedArrayBuffer), in the order they are sent
pub const ISOLATION: [(&str, &str); 3] = [
    ("cross-origin-opener-policy", "same-origin"),
    ("cross-origin-embedder-policy", "require-corp"),
    ("cross-origin-resource-policy", "same-origin"),
];

/// Strict-Transport-Security: a year, where the server asks for it (`hsts` of [`security_headers`])
pub const HSTS: &str = "max-age=31536000";

/// The headers every response carries, with the default CSP, in the order they are sent; HSTS where `hsts`, and
/// X-Robots-Tag "noindex, nofollow" where `noindex` ("index, follow" otherwise).
pub fn security_headers(hsts: bool, noindex: bool) -> Vec<(&'static str, String)> {
    let mut h = Vec::with_capacity(10);
    h.extend(ISOLATION.iter().map(|&(k, v)| (k, v.to_string())));
    h.push(("content-security-policy", CSP.to_string()));
    h.push(("x-content-type-options", "nosniff".to_string()));
    h.push(("referrer-policy", "no-referrer".to_string()));
    h.push(("x-frame-options", "DENY".to_string()));
    h.push(("permissions-policy", PERMISSIONS_POLICY.to_string()));
    h.push(("x-robots-tag", if noindex { "noindex, nofollow" } else { "index, follow" }.to_string()));
    if hsts {
        h.push(("strict-transport-security", HSTS.to_string()));
    }
    h
}

// ---- caching -------------------------------------------------------------------

macro_rules! edge_keep {
    () => {
        "public, max-age=31536000, immutable"
    };
}
/// what every cache, the CDN's too, may keep for good
pub const EDGE_KEEP: &str = edge_keep!();

/// Who may keep a response, for [`cache_headers`].  Shared caches keep nothing: 'private' says so to every
/// cache, and CDN-Cache-Control / Cloudflare-CDN-Cache-Control speak to the CDN alone.  Two exceptions: a run of the
/// game data whose address names the file's version may stay at the CDN for good (the server's `send_blocks` sets
/// EDGE_KEEP on its CDN headers), and so may the module at the address that names its content (`Immutable`).
/// no-transform: a proxy hands the bytes on as they are (the brotli module, the byte ranges of the game files).
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Cache {
    /// the browser keeps nothing
    Store,
    /// the browser keeps a copy it must revalidate each time (the page's scripts, which 25 workers load)
    Revalidate,
    /// an address that names its content (the game module's /game.wasm?v=<hash>), which every cache, the CDN's too,
    /// keeps for good: a start reads the 65 MB module and Chrome's compiled code for it from the disk cache without
    /// asking, and a new build is a new address
    Immutable,
}

/// Cache-Control, CDN-Cache-Control and Cloudflare-CDN-Cache-Control for a [`Cache`], in that order
pub fn cache_headers(kind: Cache) -> Vec<(&'static str, &'static str)> {
    match kind {
        Cache::Immutable => vec![
            ("cache-control", concat!(edge_keep!(), ", no-transform")),
            ("cdn-cache-control", EDGE_KEEP),
            ("cloudflare-cdn-cache-control", EDGE_KEEP),
        ],
        Cache::Store | Cache::Revalidate => vec![
            (
                "cache-control",
                if kind == Cache::Revalidate { "private, no-cache, no-transform" } else { "private, no-store, no-transform" },
            ),
            ("cdn-cache-control", "no-store"),
            ("cloudflare-cdn-cache-control", "no-store"),
        ],
    }
}

/// Of the encodings the server has (in its order of preference), the first that an Accept-Encoding header takes
/// (a q=0 refuses one; "*" is not read as taking any), or None.
pub fn pick_encoding<'a>(header: Option<&str>, have: &[&'a str]) -> Option<&'a str> {
    let lower = header.unwrap_or("").to_lowercase();
    let mut ok: Vec<&str> = Vec::new();
    for part in lower.split(',') {
        let mut pieces = part.split(';');
        let name = js::trim(pieces.next().unwrap_or(""));
        if !pieces.any(|p| q_zero(js::trim(p))) {
            ok.push(name);
        }
    }
    have.iter().copied().find(|e| ok.contains(e))
}

/// `/^q=0(\.0{0,3})?$/` once the parameter's white space (JS's \s) is gone: "q=0", "q = 0.000"
fn q_zero(param: &str) -> bool {
    let mut it = param.chars().filter(|&c| !js::is_space(c));
    if !(it.next() == Some('q') && it.next() == Some('=') && it.next() == Some('0')) {
        return false;
    }
    match it.next() {
        None => true,
        // up to three zeros after the point
        Some('.') => it.try_fold(0, |zeros, c| (c == '0' && zeros < 3).then_some(zeros + 1)).is_some(),
        Some(_) => false,
    }
}

/// If-None-Match against an ETag, as a browser or a proxy (which may make it weak) sends it
pub fn etag_matches(header: Option<&str>, etag: &str) -> bool {
    let Some(header) = header.filter(|h| !h.is_empty()) else { return false };
    let bare = etag.strip_prefix("W/").unwrap_or(etag);
    header.split(',').map(js::trim).any(|t| t == "*" || t.strip_prefix("W/").unwrap_or(t) == bare)
}

// ---- proxies -------------------------------------------------------------------

/// cloudflare-ips.txt (next to this crate's Cargo.toml), as it was when this was built
const CLOUDFLARE_IPS: &str = include_str!("../cloudflare-ips.txt");

/// Cloudflare's origin-pull CA (cloudflare-origin-pull-ca.pem next to this crate's Cargo.toml, PEM): what signs the
/// client certificate Cloudflare's proxy shows an origin with Authenticated Origin Pulls on ([`ProxyTrust`])
pub const CLOUDFLARE_ORIGIN_PULL_CA: &[u8] = include_bytes!("../cloudflare-origin-pull-ca.pem");

/// Cloudflare's published ranges, from cloudflare-ips.txt next to this crate's Cargo.toml (built in; `file` reads
/// another): a line each, '#' starts a comment.
pub fn cloudflare_ranges(file: Option<&Path>) -> std::io::Result<Vec<String>> {
    let text = match file {
        None => Cow::Borrowed(CLOUDFLARE_IPS),
        Some(f) => Cow::Owned(String::from_utf8_lossy(&std::fs::read(f)?).into_owned()),
    };
    Ok(ranges_of(&text))
}

/// The lines (split at `\r?\n`), each without `#.*` and trimmed (JS's `trim`), the empty ones left out
fn ranges_of(text: &str) -> Vec<String> {
    let mut lines: Vec<&str> = text.split('\n').collect();
    let last = lines.len() - 1;
    for l in &mut lines[..last] {
        *l = l.strip_suffix('\r').unwrap_or(l);
    }
    lines
        .into_iter()
        .map(|l| {
            // '.' stops at a line terminator: what follows a lone \r (or U+2028/9) stays
            let Some(p) = l.find('#') else { return js::trim(l).to_string() };
            let end = l[p..].find(['\n', '\r', '\u{2028}', '\u{2029}']).map_or(l.len(), |e| p + e);
            js::trim(&format!("{}{}", &l[..p], &l[end..])).to_string()
        })
        .filter(|l| !l.is_empty())
        .collect()
}

/// This machine: a proxy or tunnel here (Caddy, nginx, cloudflared) is always trusted.
const LOOPBACK: [&str; 2] = ["127.0.0.0/8", "::1/128"];

/// An address in its plain form, lower case: "::ffff:1.2.3.4" -> "1.2.3.4", "[::1]" -> "::1", "fe80::1%en0" ->
/// "fe80::1"; "" if not an address
pub fn bare_ip(a: &str) -> String {
    ip::bare(a).map(|(s, _)| s.to_ascii_lowercase()).unwrap_or_default()
}

/// Node's `net.isIP(s)`: 4, 6 or 0 (Node 24's regexes: IPv4 without leading zeros, IPv6 with an optional zone)
pub fn is_ip(s: &str) -> u8 {
    ip::is_ip(s)
}

/// Node's `net.isIPv4(s)`
pub fn is_ipv4(s: &str) -> bool {
    ip::v4(s).is_some()
}

/// Node's `net.isIPv6(s)`
pub fn is_ipv6(s: &str) -> bool {
    ip::is_v6(s)
}

/// What the per-address limits count by: an IPv4 address, or an IPv6 address's /56 (a home or a VPS gets a /64 at
/// least and often a /56, so counting single IPv6 addresses, or /64s, would give one client 2^64, or 256, budgets).
/// "" when it is not an address.
pub fn address_key(ip: &str) -> String {
    match ip::bare(ip) {
        None => String::new(),
        Some((s, Ip::V4(_))) => s.to_string(),
        Some((_, Ip::V6(g))) => format!("{:x}:{:x}:{:x}:{:x}::/56", g[0], g[1], g[2], g[3] & 0xff00),
    }
}

/// The wider group the limits also count by: an IPv6 address's /48 (one site, or one free tunnel-broker allocation:
/// 256 /56s, which would otherwise be 256 budgets for one client); "" for IPv4 (an address is its own group).
pub fn site_key(ip: &str) -> String {
    match ip::bare(ip) {
        Some((_, Ip::V6(g))) => format!("{:x}:{:x}:{:x}::/48", g[0], g[1], g[2]),
        _ => String::new(),
    }
}

// ---- the server's own names ------------------------------------------------------

/// The host of a Host header or an Origin ("name", "name:port", "[v6]:port", an IPv4 literal): lower case, without
/// the port, brackets or a trailing dot; "" when it is none of these.  An IPv6 host is written as the URL parser
/// writes it ("[::FFFF:1.2.3.4]" is "::ffff:102:304").
pub fn host_name(h: &str) -> String {
    // JS's toLowerCase, Unicode's: the Kelvin sign is a 'k' after it
    let s = js::trim(h).to_lowercase();
    if s.is_empty() {
        return String::new();
    }
    if let Some(rest) = s.strip_prefix('[') {
        let Some(e) = rest.find(']') else { return String::new() };
        if !is_port_or_nothing(&rest[e + 1..]) {
            return String::new();
        }
        let inner = &rest[..e];
        return if ip::is_v6(inner) { ip::canon6(inner) } else { String::new() };
    }
    if ip::is_v6(&s) {
        return ip::canon6(&s);
    }
    // /^([^:]+)(?::\d{1,5})?$/
    let (name, port) = match s.split_once(':') {
        Some((n, p)) => (n, Some(p)),
        None => (&s[..], None),
    };
    if name.is_empty() || port.is_some_and(|p| !is_port(p)) {
        return String::new();
    }
    let name = name.strip_suffix('.').unwrap_or(name);
    if ip::v4(name).is_some() || is_dns_name(name) { name.to_string() } else { String::new() }
}

/// `\d{1,5}`
fn is_port(p: &str) -> bool {
    (1..=5).contains(&p.len()) && p.bytes().all(|c| c.is_ascii_digit())
}

/// `/^(:\d{1,5})?$/`: what may follow "[v6]"
fn is_port_or_nothing(s: &str) -> bool {
    s.is_empty() || s.strip_prefix(':').is_some_and(is_port)
}

/// `/^[a-z0-9_]([a-z0-9_-]{0,62})(\.[a-z0-9_]([a-z0-9_-]{0,62}))*$/`: dot-separated labels of 1 to 63 characters
fn is_dns_name(s: &str) -> bool {
    s.split('.').all(|label| {
        let b = label.as_bytes();
        let inner = |c: &u8| c.is_ascii_lowercase() || c.is_ascii_digit() || *c == b'_';
        (1..=63).contains(&b.len()) && inner(&b[0]) && b[1..].iter().all(|c| inner(c) || *c == b'-')
    })
}

/// this machine's own names, which every check takes
fn is_loopback_name(n: &str) -> bool {
    n == "localhost" || n == "::1" || (ip::v4(n).is_some() && n.starts_with("127."))
}

/// Localhost or an IP literal (with or without a port): HSTS means nothing (or harm) there.  A bare "::1" is not
/// taken (its ":1" reads as a port); a Host header brackets an IPv6 literal.
pub fn is_local_or_ip(host_header: Option<&str>) -> bool {
    let lower = js::trim(host_header.unwrap_or("")).to_lowercase();
    if lower.starts_with('[') {
        // [v6]:port
        return true;
    }
    // /:\d+$/, then /\.$/
    let mut h = &lower[..];
    if let Some(p) = h.rfind(':') {
        let digits = &h[p + 1..];
        if !digits.is_empty() && digits.bytes().all(|c| c.is_ascii_digit()) {
            h = &h[..p];
        }
    }
    let h = h.strip_suffix('.').unwrap_or(h);
    h == "localhost" || h.ends_with(".localhost") || ip::is_ip(h) != 0
}

/// The names the server answers for (Host, Origin).  [`HostCheck::check`]: the host is one of the server's own
/// names: this machine (localhost, 127.0.0.0/8, ::1) always, the given names (exact, or "*.zone" for one label under
/// zone), and the names of its certificate (`set_cert_names`, replaced when the certificate is).  Thread-safe: the
/// certificate's names may be replaced while requests are checked.
pub struct HostCheck {
    /// the given names, in lower case
    fixed: Vec<String>,
    names: RwLock<Names>,
}

/// the names one check reads: exact ones (in the order given, for `names()`) and ".zone"s
#[derive(Default)]
struct Names {
    exact: HashSet<String>,
    order: Vec<String>,
    wild: Vec<String>,
}

impl HostCheck {
    pub fn new(names: &[String]) -> Self {
        let fixed: Vec<String> = names.iter().map(|n| n.to_lowercase()).collect();
        let names = RwLock::new(Self::build(&fixed, &[]));
        HostCheck { fixed, names }
    }

    fn build(fixed: &[String], cert: &[String]) -> Names {
        let mut names = Names::default();
        for n in fixed.iter().chain(cert) {
            if n.starts_with("*.") {
                // ".zone"
                names.wild.push(n[1..].to_string());
                continue;
            }
            let h = if n.contains(':') && !n.starts_with('[') && ip::is_v6(n) { host_name(&format!("[{n}]")) } else { host_name(n) };
            if !h.is_empty() && names.exact.insert(h.clone()) {
                names.order.push(h);
            }
        }
        names
    }

    /// the host of a Host header (or an Origin) is one of the server's own names
    pub fn check(&self, host: Option<&str>) -> bool {
        let n = host_name(host.unwrap_or(""));
        if n.is_empty() {
            return false;
        }
        if is_loopback_name(&n) {
            return true;
        }
        let names = self.names.read();
        names.exact.contains(&n)
            || names.wild.iter().any(|w| n.len() > w.len() && n.ends_with(w.as_str()) && !n[..n.len() - w.len()].contains('.'))
    }

    /// the certificate's names, in place of the last certificate's
    pub fn set_cert_names(&self, list: &[String]) {
        let cert: Vec<String> = list.iter().map(|n| n.to_lowercase()).collect();
        let names = Self::build(&self.fixed, &cert);
        *self.names.write() = names;
    }

    /// the exact names, then the wildcards ("*.zone")
    pub fn names(&self) -> Vec<String> {
        let names = self.names.read();
        names.order.iter().cloned().chain(names.wild.iter().map(|w| format!("*{w}"))).collect()
    }
}

/// Which TCP peers are proxies whose forwarding headers are believed (only those).  The spec (--trusted-proxies, for
/// [`ProxyTrust::parse`]): a comma list of 'cloudflare' (Cloudflare's edge ranges, for a server that Cloudflare's
/// proxy reaches directly), IP addresses and CIDR ranges.  This machine (loopback: a proxy or tunnel here, such as
/// Caddy, nginx or cloudflared) is always trusted.
///
/// Cloudflare's ranges are not Cloudflare's proxy alone: any of its customers connects from them too (a Worker's TCP
/// socket), and could say any client address.  So a peer there must also show Cloudflare's origin-pull certificate
/// in its TLS handshake ([`ProxyTrust::must_prove`], [`CLOUDFLARE_ORIGIN_PULL_CA`]: Authenticated Origin Pulls), which
/// only Cloudflare's proxy has (the second security audit, 2026-10-22).
pub struct ProxyTrust {
    /// the Cloudflare ranges are in
    pub cloudflare: bool,
    /// what was given (for the start-up line)
    pub entries: Vec<String>,
    rules: Vec<Rule>,
    /// the Cloudflare ranges among them
    shared: Vec<Rule>,
}

/// One entry of Node's BlockList, on the line both families share there: an IPv4 address or range matches as its
/// IPv4-mapped IPv6 one (1.2.3.0/24 is ::ffff:1.2.3.0/120), so "::ffff:7f00:1" is in 127.0.0.0/8 and 1.2.3.4 is in
/// ::ffff:0:0/96, but 127.0.0.1 is not ::1.  An address is a /128.
#[derive(Copy, Clone, Debug)]
struct Rule {
    net: u128,
    prefix: u32,
}

impl Rule {
    fn new(ip: Ip, bits: Option<u32>) -> Rule {
        let prefix = match ip {
            Ip::V4(_) => 96 + bits.unwrap_or(32),
            Ip::V6(_) => bits.unwrap_or(128),
        };
        Rule { net: ip.as_v6(), prefix }
    }

    fn has(&self, a: u128) -> bool {
        self.prefix == 0 || (a ^ self.net) >> (128 - self.prefix) == 0
    }
}

impl ProxyTrust {
    /// Err: the message the server dies with at start-up
    pub fn parse(spec: &str) -> Result<Self, String> {
        let mut t = ProxyTrust { cloudflare: false, entries: Vec::new(), rules: Vec::new(), shared: Vec::new() };
        for it in LOOPBACK {
            t.add(it, it)?;
        }
        for raw in spec.split(',').map(js::trim).filter(|s| !s.is_empty()) {
            if raw == "loopback" || raw == "none" {
                continue;
            }
            if raw == "cloudflare" {
                let from = t.rules.len();
                for r in cloudflare_ranges(None).map_err(|e| e.to_string())? {
                    t.add(&r, &r)?;
                }
                t.shared.extend_from_slice(&t.rules[from..]);
                t.cloudflare = true;
                t.entries.push(raw.to_string());
                continue;
            }
            t.add(raw, raw)?;
            t.entries.push(raw.to_string());
        }
        Ok(t)
    }

    /// an address ("1.2.3.4", "[::1]") or a CIDR range ("10.0.0.0/8"); its prefix counts in its own family
    fn add(&mut self, it: &str, from: &str) -> Result<(), String> {
        let mut parts = it.split('/');
        let addr = parts.next().unwrap_or("");
        let (bits, more) = (parts.next(), parts.next());
        let bad = || format!("--trusted-proxies: \"{from}\" is not an address, a CIDR range or \"cloudflare\"");
        let Some((_, ip)) = ip::bare(addr) else { return Err(bad()) };
        let max = if matches!(ip, Ip::V4(_)) { 32 } else { 128 };
        if more.is_some() {
            return Err(bad());
        }
        let bits = match bits {
            None => None,
            Some(b) => {
                // /^\d{1,3}$/ and at most the family's width
                if !(1..=3).contains(&b.len()) || !b.bytes().all(|c| c.is_ascii_digit()) {
                    return Err(bad());
                }
                let n: u32 = b.parse().map_err(|_| bad())?;
                if n > max {
                    return Err(bad());
                }
                Some(n)
            }
        };
        self.rules.push(Rule::new(ip, bits));
        Ok(())
    }

    /// the TCP peer (in any form [`bare_ip`] reads) is this machine or a given proxy
    pub fn trusts(&self, peer: &str) -> bool {
        ip::bare(peer).is_some_and(|(_, ip)| self.trusts_ip(ip))
    }

    /// the TCP peer is in Cloudflare's ranges (`cloudflare` given): its TLS handshake must show Cloudflare's
    /// origin-pull certificate, or it fails (the type's doc)
    pub fn must_prove(&self, peer: &str) -> bool {
        ip::bare(peer).is_some_and(|(_, ip)| {
            let a = ip.as_v6();
            self.shared.iter().any(|r| r.has(a))
        })
    }

    fn trusts_ip(&self, ip: Ip) -> bool {
        let a = ip.as_v6();
        self.rules.iter().any(|r| r.has(a))
    }
}

/// The client's address (header: "x-forwarded-for" or "cf-connecting-ip", the --real-ip-header): the TCP peer's,
/// unless the peer is a trusted proxy.  Then the address it reports: header 'cf-connecting-ip' (Cloudflare's proxy
/// and cloudflared set it and replace what a client sent), else the right-most X-Forwarded-For entry that is not
/// itself a trusted proxy.  A header from a peer that is not trusted is never read.
pub fn client_ip(q: &impl Req, trust: &ProxyTrust, header: &str) -> String {
    let Some((peer, peer_ip)) = ip::bare(q.peer()) else { return String::new() };
    if !trust.trusts_ip(peer_ip) {
        return peer.to_ascii_lowercase();
    }
    if header == "cf-connecting-ip" {
        let first = q.header("cf-connecting-ip").unwrap_or("").split(',').next().unwrap_or("");
        if let Some((cf, _)) = ip::bare(first) {
            return cf.to_ascii_lowercase();
        }
    }
    let xff = q.header("x-forwarded-for").unwrap_or("");
    for entry in xff.rsplit(',').map(js::trim).filter(|s| !s.is_empty()) {
        let Some((s, ip)) = ip::bare(entry) else { break };
        if !trust.trusts_ip(ip) {
            return s.to_ascii_lowercase();
        }
    }
    peer.to_ascii_lowercase()
}

/// The browser reached us over https: directly, or through a trusted proxy that says so (X-Forwarded-Proto, else
/// Cloudflare's CF-Visitor).
pub fn via_https(q: &impl Req, trust: &ProxyTrust) -> bool {
    if q.encrypted() {
        return true;
    }
    if !trust.trusts(q.peer()) {
        return false;
    }
    let proto = js::trim(q.header("x-forwarded-proto").unwrap_or("").split(',').next().unwrap_or("")).to_lowercase();
    if !proto.is_empty() {
        return proto == "https";
    }
    visitor::scheme_is_https(q.header("cf-visitor").unwrap_or("{}"))
}

/// The host the browser asked for, when a trusted proxy rewrote Host (X-Forwarded-Host).
pub fn forwarded_host(q: &impl Req, trust: &ProxyTrust) -> Option<String> {
    if !trust.trusts(q.peer()) {
        return None;
    }
    let h = js::trim(q.header("x-forwarded-host").unwrap_or("").split(',').next().unwrap_or("")).to_lowercase();
    (!h.is_empty()).then_some(h)
}

#[cfg(test)]
mod tests;
