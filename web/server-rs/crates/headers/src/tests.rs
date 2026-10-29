//! The rules of this crate, case by case from its comments.

use std::collections::HashMap;

use super::*;

struct Q {
    peer: &'static str,
    encrypted: bool,
    headers: HashMap<&'static str, &'static str>,
}

impl Req for Q {
    fn peer(&self) -> &str {
        self.peer
    }
    fn header(&self, name: &str) -> Option<&str> {
        self.headers.get(name).copied()
    }
    fn encrypted(&self) -> bool {
        self.encrypted
    }
}

fn q(peer: &'static str, headers: &[(&'static str, &'static str)]) -> Q {
    Q { peer, encrypted: false, headers: headers.iter().copied().collect() }
}

fn strings(list: &[&str]) -> Vec<String> {
    list.iter().map(|s| s.to_string()).collect()
}

#[test]
fn csp_and_policies() {
    assert_eq!(content_security_policy(&[], &[]), CSP);
    assert_eq!(
        CSP,
        "default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; worker-src 'self'; connect-src 'self'; \
         img-src 'self' data: blob:; style-src 'self'; font-src 'self'; media-src 'self'; object-src 'none'; \
         base-uri 'none'; form-action 'self'; frame-src 'none'; frame-ancestors 'none'"
    );
    let with = content_security_policy(&["sha256-a", "sha256-b"], &["sha256-c"]);
    assert!(with.contains("script-src 'self' 'wasm-unsafe-eval' 'sha256-a' 'sha256-b'; worker-src"), "{with}");
    assert!(with.contains("style-src 'self' 'sha256-c'; font-src"), "{with}");
    // crypto.createHash('sha256').update('x', 'utf8').digest('base64')
    assert_eq!(sha256_source("x"), "sha256-LXEWQrcmsEQBYnyp+6wy9chTD7GQPMTbAiWHF5IaSIE=");
    assert!(permissions_policy().starts_with("gamepad=(self), fullscreen=(self), "));
    assert!(permissions_policy().ends_with(", window-management=(), xr-spatial-tracking=()"));
    assert_eq!(permissions_policy().split(", ").count(), 27);
}

#[test]
fn security_headers_in_order() {
    let h = security_headers(false, true);
    let names: Vec<&str> = h.iter().map(|(k, _)| *k).collect();
    assert_eq!(
        names,
        [
            "cross-origin-opener-policy",
            "cross-origin-embedder-policy",
            "cross-origin-resource-policy",
            "content-security-policy",
            "x-content-type-options",
            "referrer-policy",
            "x-frame-options",
            "permissions-policy",
            "x-robots-tag",
        ]
    );
    assert_eq!(h[1].1, "require-corp");
    assert_eq!(h[8].1, "noindex, nofollow");
    let h = security_headers(true, false);
    assert_eq!(h[8].1, "index, follow");
    assert_eq!(h.last().unwrap(), &("strict-transport-security", HSTS.to_string()));
}

#[test]
fn cache_headers_by_kind() {
    assert_eq!(
        cache_headers(Cache::Store),
        [
            ("cache-control", "private, no-store, no-transform"),
            ("cdn-cache-control", "no-store"),
            ("cloudflare-cdn-cache-control", "no-store")
        ]
    );
    assert_eq!(cache_headers(Cache::Revalidate)[0], ("cache-control", "private, no-cache, no-transform"));
    assert_eq!(
        cache_headers(Cache::Immutable),
        [
            ("cache-control", "public, max-age=31536000, immutable, no-transform"),
            ("cdn-cache-control", EDGE_KEEP),
            ("cloudflare-cdn-cache-control", EDGE_KEEP)
        ]
    );
}

// Accept-Encoding: the first of the server's encodings the client takes
#[test]
fn accept_encoding_first_the_client_takes() {
    let have = ["zstd", "br", "gzip"];
    assert_eq!(pick_encoding(Some("gzip, deflate, br, zstd"), &have), Some("zstd"));
    assert_eq!(pick_encoding(Some("gzip, br"), &have), Some("br")); // Cloudflare's edge to an origin
    assert_eq!(pick_encoding(Some("GZIP"), &have), Some("gzip"));
    assert_eq!(pick_encoding(Some("br;q=0, gzip"), &have), Some("gzip"));
    assert_eq!(pick_encoding(Some("zstd;q=0.0, br; q=1"), &have), Some("br"));
    assert_eq!(pick_encoding(Some("identity"), &have), None);
    assert_eq!(pick_encoding(Some("*"), &have), None);
    assert_eq!(pick_encoding(None, &have), None);
    assert_eq!(pick_encoding(Some("zstd, br"), &["br", "gzip"]), Some("br")); // a Node without zstd
    // q=0 with up to three decimals, white space anywhere in it
    assert_eq!(pick_encoding(Some("zstd; q = 0.000, br;q=0.0000"), &have), Some("br"));
    assert_eq!(pick_encoding(Some("zstd;q=0.001"), &have), Some("zstd"));
}

#[test]
fn if_none_match() {
    assert!(etag_matches(Some("\"abc\""), "\"abc\""));
    assert!(etag_matches(Some("W/\"abc\""), "\"abc\"")); // a proxy made it weak
    assert!(etag_matches(Some("\"abc\""), "W/\"abc\""));
    assert!(etag_matches(Some("\"x\", \"abc\""), "\"abc\""));
    assert!(etag_matches(Some(" * "), "\"abc\""));
    assert!(!etag_matches(Some("\"ab\""), "\"abc\""));
    assert!(!etag_matches(Some("w/\"abc\""), "\"abc\""));
    assert!(!etag_matches(Some(""), "\"abc\""));
    assert!(!etag_matches(None, "\"abc\""));
}

#[test]
fn cloudflare_ranges_built_in() {
    let r = cloudflare_ranges(None).unwrap();
    assert!(r.contains(&"173.245.48.0/20".to_string()));
    assert!(r.contains(&"2606:4700::/32".to_string()));
    assert!(r.iter().all(|l| !l.contains('#') && !l.is_empty()));
    assert_eq!(
        ranges_of("# a\r\n1.2.3.0/24 # b\r\n\r\n \u{feff}5.6.7.0/24\n9.9.9.9#x\ry\nlast\r"),
        ["1.2.3.0/24", "5.6.7.0/24", "9.9.9.9\ry", "last"]
    );
}

#[test]
fn bare_ip_examples() {
    assert_eq!(bare_ip("::ffff:1.2.3.4"), "1.2.3.4");
    assert_eq!(bare_ip("::FFFF:10.0.0.1"), "10.0.0.1");
    assert_eq!(bare_ip("[::1]"), "::1");
    assert_eq!(bare_ip("fe80::1%en0"), "fe80::1");
    assert_eq!(bare_ip(" 2001:DB8::1 "), "2001:db8::1");
    assert_eq!(bare_ip("::ffff:7f00:1"), "::ffff:7f00:1"); // the hex form stays IPv6
    for not in ["", "garbage", "1.2.3", "01.2.3.4", "256.1.1.1", "::ffff:1.2.3", "2001::db8::1", "[::1]:80", "%1"] {
        assert_eq!(bare_ip(not), "", "{not}");
    }
    assert_eq!(is_ip("1.2.3.4"), 4);
    assert_eq!(is_ip("fe80::1%eth0"), 6);
    assert_eq!(is_ip("[::1]"), 0);
}

#[test]
fn address_and_site_keys() {
    // an IPv6 client counts by its /56, and its site by its /48
    assert_eq!(address_key("2001:db8:1:2ff::1"), "2001:db8:1:200::/56");
    assert_eq!(address_key("2001:db8:1:2ff:ffff:ffff:ffff:ffff"), "2001:db8:1:200::/56");
    assert_eq!(site_key("2001:db8:1:2ff::1"), "2001:db8:1::/48");
    assert_eq!(address_key("[2001:0DB8:0001:00ff::]"), "2001:db8:1:0::/56");
    assert_eq!(address_key("::1"), "0:0:0:0::/56");
    assert_eq!(site_key("::1.2.3.4"), "0:0:0::/48");
    // an IPv4 address is its own key, and has no wider group
    assert_eq!(address_key("::ffff:1.2.3.4"), "1.2.3.4");
    assert_eq!(site_key("1.2.3.4"), "");
    assert_eq!(address_key("garbage"), "");
    assert_eq!(site_key("garbage"), "");
}

#[test]
fn host_names() {
    for (h, n) in [
        ("localhost", "localhost"),
        ("Game.Example.Com.:443", "game.example.com"),
        ("127.0.0.1:8527", "127.0.0.1"),
        ("1.2.3.4:99999", "1.2.3.4"),
        ("[::1]:8080", "::1"),
        ("[2001:DB8:0::1]", "2001:db8::1"),
        ("2001:db8::1", "2001:db8::1"),
        ("[::FFFF:1.2.3.4]:443", "::ffff:102:304"),
        ("under_score.example", "under_score.example"),
        ("a-.example", "a-.example"),
        ("\u{212A}elvin.example", "kelvin.example"),
        (" x.example ", "x.example"),
        ("[fe80::1%25eth0]", ""),
        ("fe80::1%eth0", ""),
        ("1.2.3.4:123456", ""),
        ("x:y", ""),
        ("[::1", ""),
        ("[::1]x", ""),
        ("[::1]:", ""),
        ("[1.2.3.4]", ""),
        ("-bad.example", ""),
        ("a..b", ""),
        ("a.b..", ""),
        ("é.example", ""),
        ("", ""),
    ] {
        assert_eq!(host_name(h), n, "{h:?}");
    }
    let long = "x".repeat(63);
    assert_eq!(host_name(&format!("{long}.com")), format!("{long}.com"));
    assert_eq!(host_name(&format!("{long}x.com")), "");
}

#[test]
fn local_or_ip() {
    for yes in ["localhost:8080", "LOCALHOST.", "a.localhost:80", "[::1]:80", "[anything", "1.2.3.4:443", "fe80::1%eth0", "2001:db8::abc"] {
        assert!(is_local_or_ip(Some(yes)), "{yes}");
    }
    // a bare IPv6 address that ends in digits loses them as a "port" (a Host header brackets one)
    for no in ["example.com", "::1", "2001:db8::1", "", "localhost.example"] {
        assert!(!is_local_or_ip(Some(no)), "{no}");
    }
    assert!(!is_local_or_ip(None));
}

#[test]
fn host_check() {
    let check = HostCheck::new(&strings(&["game.example.com", "*.zone.test", "2001:DB8::1"]));
    for yes in [
        "game.example.com:443",
        "GAME.example.com.",
        "a.zone.test",
        "localhost:1",
        "127.9.9.9",
        "[::1]:80",
        "[2001:db8::1]",
        "[2001:db8:0::1]:8",
    ] {
        assert!(check.check(Some(yes)), "{yes}");
    }
    // one label under the zone, not the zone itself
    for no in ["b.a.zone.test", "zone.test", ".zone.test", "evil.example", "", "[::2]"] {
        assert!(!check.check(Some(no)), "{no}");
    }
    assert!(!check.check(None));
    assert_eq!(check.names(), ["game.example.com", "2001:db8::1", "*.zone.test"]);
    check.set_cert_names(&strings(&["Cert.Example", "*.Cert.Example", "10.0.0.1"]));
    assert!(check.check(Some("cert.example")) && check.check(Some("x.cert.example")) && check.check(Some("10.0.0.1:443")));
    assert_eq!(check.names(), ["game.example.com", "2001:db8::1", "cert.example", "10.0.0.1", "*.zone.test", "*.cert.example"]);
    // a new certificate replaces the names of the last
    check.set_cert_names(&[]);
    assert!(!check.check(Some("cert.example")));
    assert!(check.check(Some("game.example.com")));
}

#[test]
fn host_check_while_the_certificate_changes() {
    let check = std::sync::Arc::new(HostCheck::new(&strings(&["game.example.com"])));
    let readers: Vec<_> = (0..4)
        .map(|_| {
            let c = check.clone();
            std::thread::spawn(move || {
                for _ in 0..20_000 {
                    assert!(c.check(Some("game.example.com")));
                    assert!(!c.check(Some("evil.example")));
                }
            })
        })
        .collect();
    for i in 0..2_000 {
        check.set_cert_names(&[format!("cert{i}.example")]);
    }
    for r in readers {
        r.join().unwrap();
    }
    assert!(check.check(Some("cert1999.example")));
}

#[test]
fn proxy_trust_spec() {
    let t = ProxyTrust::parse("").unwrap();
    assert!(!t.cloudflare && t.entries.is_empty());
    // this machine always
    for yes in ["127.0.0.1", "127.255.0.1", "::1", "[::1]", "::ffff:127.0.0.1", "0:0:0:0:0:0:0:1"] {
        assert!(t.trusts(yes), "{yes}");
    }
    for no in ["10.0.0.1", "", "garbage", "::2"] {
        assert!(!t.trusts(no), "{no}");
    }
    let t = ProxyTrust::parse("cloudflare, 10.0.0.0/8 , loopback,none,, 2001:db8::/32,[2001:db8:1::1]").unwrap();
    assert!(t.cloudflare);
    assert_eq!(t.entries, ["cloudflare", "10.0.0.0/8", "2001:db8::/32", "[2001:db8:1::1]"]);
    for yes in ["173.245.48.1", "2606:4700::1", "10.1.2.3", "2001:db8:ffff::1"] {
        assert!(t.trusts(yes), "{yes}");
    }
    for no in ["11.0.0.1", "2001:db9::1"] {
        assert!(!t.trusts(no), "{no}");
    }
    for bad in ["foo", "1.2.3.4/33", "::1/129", "1.2.3.4/", "1.2.3.4/8/1", "1.2.3.4/ 8", "Cloudflare", "::ffff:1.2.3.4/120", "/8"] {
        assert_eq!(
            ProxyTrust::parse(&format!("10.0.0.1, {bad} ")).err().as_deref(),
            Some(&*format!("--trusted-proxies: \"{bad}\" is not an address, a CIDR range or \"cloudflare\"")),
            "{bad}"
        );
    }
}

// What Node's BlockList does across the families (by experiment, node 24): an IPv4 address is its IPv4-mapped
// IPv6 address, and an IPv4 range's prefix counts in IPv4 bits.
#[test]
fn proxy_trust_as_node_blocklist() {
    let trusts = |spec: &str, peer: &str| ProxyTrust::parse(spec).unwrap().trusts(peer);
    assert!(trusts("", "::ffff:7f00:1"));
    assert!(!trusts("", "::7f00:1"));
    assert!(trusts("1.2.3.0/24", "::ffff:102:363"));
    assert!(!trusts("1.2.3.0/24", "::ffff:102:463"));
    assert!(trusts("::ffff:0:0/96", "9.9.9.9"));
    assert!(trusts("::/0", "9.9.9.9"));
    assert!(trusts("::/80", "9.9.9.9"));
    assert!(!trusts("::/81", "9.9.9.9"));
    assert!(!trusts("::/96", "9.9.9.9"));
    assert!(!trusts("0.0.0.0/0", "2001:db8::1"));
    assert!(trusts("0.0.0.0/0", "::ffff:5.5.5.5"));
    assert!(trusts("::ffff:a00:1", "10.0.0.1"));
    assert!(trusts("10.0.0.1", "::ffff:a00:1"));
    assert!(!trusts("::a00:1", "10.0.0.1"));
    assert!(!trusts("0.0.0.0", "::"));
}

/// Cloudflare's addresses, and only those, must show its origin-pull certificate: a proxy given by its own address,
/// this machine and everyone else need not (the second security audit)
#[test]
fn only_cloudflares_addresses_must_prove_themselves() {
    let t = ProxyTrust::parse("cloudflare, 10.0.0.0/8").unwrap();
    for cf in ["104.16.0.1", "::ffff:104.16.0.1", "[2606:4700::1]", "172.64.1.2"] {
        assert!(t.trusts(cf) && t.must_prove(cf), "{cf}");
    }
    for other in ["10.1.2.3", "127.0.0.1", "::1", "203.0.113.5", ""] {
        assert!(!t.must_prove(other), "{other}");
    }
    assert!(!ProxyTrust::parse("10.0.0.0/8").unwrap().must_prove("104.16.0.1"));
    assert!(!ProxyTrust::parse("104.16.0.0/13").unwrap().must_prove("104.16.0.1"), "a range given by hand is the operator's own");
    assert!(CLOUDFLARE_ORIGIN_PULL_CA.windows(27).any(|w| w == b"-----BEGIN CERTIFICATE-----"));
}

#[test]
fn client_ip_from_trusted_proxies_only() {
    let t = ProxyTrust::parse("10.0.0.0/8").unwrap();
    let xff = "x-forwarded-for";
    // a header from a peer that is not trusted is never read
    assert_eq!(client_ip(&q("203.0.113.5", &[(xff, "1.1.1.1")]), &t, xff), "203.0.113.5");
    assert_eq!(client_ip(&q("::ffff:203.0.113.5", &[]), &t, xff), "203.0.113.5");
    assert_eq!(client_ip(&q("", &[(xff, "1.1.1.1")]), &t, xff), "");
    // the right-most entry that is not itself a trusted proxy
    let p = q("::ffff:127.0.0.1", &[(xff, "6.6.6.6, 198.51.100.7 ,, 10.0.0.2")]);
    assert_eq!(client_ip(&p, &t, xff), "198.51.100.7");
    // an entry that is no address ends the search
    assert_eq!(client_ip(&q("127.0.0.1", &[(xff, "1.1.1.1, unknown, 10.0.0.2")]), &t, xff), "127.0.0.1");
    // Cloudflare's header first, where the server reads it
    let p = q("10.9.9.9", &[("cf-connecting-ip", "2001:DB8::5, 1.1.1.1"), (xff, "4.4.4.4")]);
    assert_eq!(client_ip(&p, &t, "cf-connecting-ip"), "2001:db8::5");
    assert_eq!(client_ip(&p, &t, xff), "4.4.4.4");
    let p = q("10.9.9.9", &[("cf-connecting-ip", "nonsense"), (xff, "4.4.4.4")]);
    assert_eq!(client_ip(&p, &t, "cf-connecting-ip"), "4.4.4.4");
}

#[test]
fn https_and_the_forwarded_host() {
    let t = ProxyTrust::parse("").unwrap();
    let mut p = q("203.0.113.5", &[("x-forwarded-proto", "https"), ("x-forwarded-host", "game.example.com")]);
    assert!(!via_https(&p, &t));
    assert_eq!(forwarded_host(&p, &t), None);
    p.encrypted = true;
    assert!(via_https(&p, &t));
    assert!(via_https(&q("127.0.0.1", &[("x-forwarded-proto", " HTTPS , http")]), &t));
    assert!(!via_https(&q("127.0.0.1", &[("x-forwarded-proto", "http"), ("cf-visitor", r#"{"scheme":"https"}"#)]), &t));
    assert!(via_https(&q("127.0.0.1", &[("x-forwarded-proto", " "), ("cf-visitor", r#"{"scheme":"https"}"#)]), &t));
    assert!(!via_https(&q("127.0.0.1", &[("cf-visitor", "{bad")]), &t));
    assert!(!via_https(&q("127.0.0.1", &[]), &t));
    let p = q("::1", &[("x-forwarded-host", " Game.Example.com , other")]);
    assert_eq!(forwarded_host(&p, &t).as_deref(), Some("game.example.com"));
    assert_eq!(forwarded_host(&q("::1", &[("x-forwarded-host", ", x")]), &t), None);
    assert_eq!(forwarded_host(&q("::1", &[]), &t), None);
}

// Cloudflare Turnstile's script and frame, and nothing else more than the default policy
#[test]
fn the_policy_with_turnstile() {
    let cf = "https://challenges.cloudflare.com";
    assert_eq!(
        CSP_TURNSTILE,
        CSP.replace("'wasm-unsafe-eval'", &format!("'wasm-unsafe-eval' {cf}")).replace("frame-src 'none'", &format!("frame-src {cf}"))
    );
}
