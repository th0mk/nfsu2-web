//! The routes on a running App (testkit.rs): what a client can make them hold, and give back.

use std::sync::Arc;
use std::time::Duration;

use futures_util::FutureExt;
use tokio::io::{AsyncReadExt, AsyncWriteExt};

use super::testkit::{Kit, data, data_bits, eventually, reset};

// ManifestCell::refresh: one walk of the game folder for everyone who asks meanwhile, and its manifest kept whoever
// is still there to take it.  A request that went (a reset) took the walk's result with it:
// the next ?refresh walked again, and a client that asked and reset at once kept as many walks going as it liked.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_manifest_walk_is_kept_when_its_asker_goes() {
    let kit = Kit::start(&[], |root| {
        for i in 0..300 {
            std::fs::write(root.join(format!("game/TRACKS/f{i:03}.bin")), b"x").unwrap();
        }
    })
    .await;
    let before = kit.app.current_manifest().unwrap();
    // asked for, and given up at once (the walk of 300 files is under way by then)
    let _ = kit.app.refresh_manifest().now_or_never();
    eventually("the walk's manifest in place", || !Arc::ptr_eq(&kit.app.current_manifest().unwrap(), &before)).await;
    assert_eq!(kit.app.current_manifest().unwrap().count, before.count);
}

// Every share of a budget a request takes is given back however the client goes: the requests in progress (a file
// sent piece by piece), the compressors (a run compressed chunk by chunk), the connections, and /lan's (an upgrade
// answered, and one refused for opening too fast after it took its share).
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn every_share_comes_back_when_the_client_goes() {
    // (answers larger than what the kernel's buffers take while the client reads nothing: 8 MiB that compress by a
    // tenth, and as much noise)
    let kit = Kit::start(&[], |root| {
        std::fs::write(root.join("game/TRACKS/big.bin"), data_bits(8 << 20, 7)).unwrap();
        std::fs::write(root.join("game/TRACKS/noise.bin"), data(8 << 20, false)).unwrap();
    })
    .await;
    let app = &kit.app;
    // a download read a little, then reset
    for (path, enc) in [
        ("/data/TRACKS/noise.bin", ""),
        ("/data-blocks/TRACKS/big.bin?b=0&n=8", "zstd"),
        ("/data-blocks/TRACKS/big.bin?b=0&n=8", "br"),
        ("/data-blocks/TRACKS/big.bin?b=0&n=8", "gzip"),
    ] {
        let mut s = kit.send(path, &[("Accept-Encoding", enc)]).await;
        let mut b = [0u8; 4096];
        let n = s.read(&mut b).await.unwrap();
        assert!(n > 0 && b.starts_with(b"HTTP/1.1 200 "), "{path} {enc}");
        if !enc.is_empty() {
            assert_eq!(app.compressors.count(), 1, "{path} {enc}: a compressor held while it streams");
        }
        assert_eq!(app.requests.count(), 1);
        reset(s);
        eventually("the shares back", || app.requests.count() == 0 && app.compressors.count() == 0 && app.conns.count() == 0).await;
    }
    // /lan: more upgrades than the burst allows, each closed after its answer
    let mut refused = 0;
    for _ in 0..24 {
        let mut s = tokio::net::TcpStream::connect(kit.addr).await.unwrap();
        s.write_all(
            b"GET /lan HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\
              Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
        )
        .await
        .unwrap();
        let mut b = [0u8; 64];
        let n = s.read(&mut b).await.unwrap();
        if b[..n].starts_with(b"HTTP/1.1 429 ") {
            refused += 1;
        } else {
            assert!(b[..n].starts_with(b"HTTP/1.1 101 "), "{}", String::from_utf8_lossy(&b[..n]));
        }
        drop(s);
        eventually("the /lan share back", || app.lan_conns.count() == 0).await;
    }
    assert!(refused >= 3, "the burst is 20: {refused} refused");
    eventually("the connections back", || app.conns.count() == 0).await;
}

// An open /lan WebSocket keeps its place on the port's count (and its client's connection share, which a loopback
// peer like this one, always a trusted proxy, does not take) for as long as the switch's tasks keep it open, as Node
// counted an upgraded socket until its 'close'; it comes back as the connection goes.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn an_open_lan_connection_keeps_its_place_on_the_port() {
    let kit = Kit::start_with(&[], nfsu2_http::Limits { max_connections: 1, ..Default::default() }, |_| {}).await;
    let app = &kit.app;
    let mut s = tokio::net::TcpStream::connect(kit.addr).await.unwrap();
    s.write_all(
        b"GET /lan HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\
          Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
    )
    .await
    .unwrap();
    let mut b = [0u8; 256];
    let n = s.read(&mut b).await.unwrap();
    assert!(b[..n].starts_with(b"HTTP/1.1 101 "), "{}", String::from_utf8_lossy(&b[..n]));
    tokio::time::sleep(Duration::from_millis(100)).await;
    // whether another connection gets an answer: none while the WebSocket is open, one once it is gone
    let answered = || async {
        let mut c = kit.send("/robots.txt", &[]).await;
        let mut b = [0u8; 64];
        matches!(tokio::time::timeout(Duration::from_secs(2), c.read(&mut b)).await, Ok(Ok(n)) if n > 0)
    };
    assert!(!answered().await, "the port's one place is the WebSocket's");
    assert_eq!(app.lan_conns.count(), 1);
    drop(s);
    eventually("the /lan share back", || app.lan_conns.count() == 0).await;
    let t0 = std::time::Instant::now();
    while !answered().await {
        assert!(t0.elapsed() < Duration::from_secs(5), "the port's place back");
    }
}

/// The answer of a /data-blocks/ run, de-chunked and decoded.
fn decoded(enc: &str, z: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    match enc {
        "zstd" => out = zstd::stream::decode_all(z).unwrap(),
        "br" => {
            std::io::Read::read_to_end(&mut brotli::Decompressor::new(z, 4096), &mut out).unwrap();
        }
        "gzip" => {
            std::io::Read::read_to_end(&mut flate2::read::GzDecoder::new(z), &mut out).unwrap();
        }
        _ => out = z.to_vec(),
    }
    out
}

// the runs as they are on disk, in every encoding, and as they are where they do not compress
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn runs_come_back_exact() {
    let kit = Kit::start(&[], |root| {
        let mut b = data(3 << 20, false);
        b.extend(data((2 << 20) + 12345, true));
        std::fs::write(root.join("game/TRACKS/mixed.bin"), b).unwrap();
    })
    .await;
    let raw = std::fs::read(kit.path("game/TRACKS/mixed.bin")).unwrap();
    for enc in ["zstd", "br", "gzip", ""] {
        for (b, n) in [(0u64, 3u64), (3, 2), (2, 8), (4, 1)] {
            let a = kit.get(&format!("/data-blocks/TRACKS/mixed.bin?b={b}&n={n}"), &[("Accept-Encoding", enc)]).await;
            assert_eq!(a.status, 200);
            let want = &raw[(b << 20) as usize..raw.len().min(((b + n) << 20) as usize)];
            let got_enc = a.header("content-encoding").unwrap_or("");
            assert_eq!(decoded(got_enc, &a.body), want, "b={b} n={n} {enc}");
            // the noise of the first 3 MiB is sent as it is
            assert_eq!(got_enc, if b < 3 || enc.is_empty() { "" } else { enc }, "b={b} n={n} {enc}");
        }
    }
    tokio::time::sleep(Duration::from_millis(50)).await;
    assert_eq!(kit.app.compressors.count(), 0);
}

/// a game file's m and s, as the manifest names them
fn version(kit: &Kit, rel: &str) -> String {
    let md = std::fs::metadata(kit.path(&format!("game/{rel}"))).unwrap();
    format!("m={}&s={}", super::etag::num(super::etag::mtime_s(&md)), md.len())
}

// One block to a client that takes br: compressed as it is sent and kept by a shared cache for a while only, until
// its block is kept at brotli's best; then that, with its length, kept for good.  Noise is sent as it is, a run of
// more blocks and a client without br as before.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_block_goes_out_as_kept_once_it_is() {
    let kit = Kit::start_keeping(1, |_| {}).await;
    // a file the manifest does not have yet: only the answers queue its blocks
    let mut raw = data(1 << 20, false);
    raw.extend(data((1 << 20) + 777, true));
    std::fs::write(kit.path("game/TRACKS/late.bin"), &raw).unwrap();
    let v = version(&kit, "TRACKS/late.bin");
    let block = |b: usize| raw[b << 20..raw.len().min((b + 1) << 20)].to_vec();
    let all = [("Accept-Encoding", "zstd, br, gzip")];
    let a = kit.get(&format!("/data-blocks/v2/TRACKS/late.bin?b=1&n=1&{v}"), &all).await;
    assert_eq!(a.status, 200);
    assert_eq!(a.header("content-encoding"), Some("zstd"), "compressed as it is sent");
    assert_eq!(a.header("cloudflare-cdn-cache-control"), Some(super::blocks::EDGE_WHILE));
    assert_eq!(decoded("zstd", &a.body), block(1));
    let mut kept = None;
    let t0 = std::time::Instant::now();
    while kept.is_none() {
        assert!(t0.elapsed() < Duration::from_secs(20), "block 1 kept");
        tokio::time::sleep(Duration::from_millis(50)).await;
        let a = kit.get(&format!("/data-blocks/v2/TRACKS/late.bin?b=1&n=1&{v}"), &all).await;
        if a.header("content-encoding") == Some("br") {
            kept = Some(a);
        }
    }
    let a = kept.unwrap();
    assert_eq!(a.header("content-length"), Some(a.body.len().to_string().as_str()));
    assert_eq!(a.header("cloudflare-cdn-cache-control"), Some(nfsu2_headers::EDGE_KEEP));
    assert_eq!(a.header("cdn-cache-control"), Some(nfsu2_headers::EDGE_KEEP));
    assert_eq!(decoded("br", &a.body), block(1));
    // the old address, the same bytes; one that names no version: never kept by a shared cache
    let old = kit.get(&format!("/data-blocks/TRACKS/late.bin?b=1&n=1&{v}"), &all).await;
    assert_eq!((old.header("content-encoding"), &old.body), (Some("br"), &a.body));
    let bare = kit.get("/data-blocks/v2/TRACKS/late.bin?b=1&n=1", &all).await;
    assert_eq!(bare.header("cloudflare-cdn-cache-control"), Some("no-store"));
    // a HEAD: the kept block's head
    let head = kit.request("HEAD", &format!("/data-blocks/v2/TRACKS/late.bin?b=1&n=1&{v}"), &all, b"").await;
    assert_eq!((head.header("content-encoding"), head.header("content-length")), (Some("br"), a.header("content-length")));
    // noise: as it is, a while, then for good once kept as one sent as it is
    let a = kit.get(&format!("/data-blocks/v2/TRACKS/late.bin?b=0&n=1&{v}"), &all).await;
    assert_eq!((a.header("content-encoding"), a.header("cloudflare-cdn-cache-control")), (None, Some(super::blocks::EDGE_WHILE)));
    assert_eq!(a.body, block(0));
    let t0 = std::time::Instant::now();
    loop {
        let a = kit.get(&format!("/data-blocks/v2/TRACKS/late.bin?b=0&n=1&{v}"), &all).await;
        assert_eq!((a.header("content-encoding"), &a.body), (None, &block(0)));
        if a.header("cloudflare-cdn-cache-control") == Some(nfsu2_headers::EDGE_KEEP) {
            break;
        }
        assert!(t0.elapsed() < Duration::from_secs(20), "block 0 kept as it is");
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    // a client without br, and a run of two: compressed as they are sent, kept for good as before
    let a = kit.get(&format!("/data-blocks/v2/TRACKS/late.bin?b=1&n=1&{v}"), &[("Accept-Encoding", "gzip")]).await;
    assert_eq!((a.header("content-encoding"), a.header("cloudflare-cdn-cache-control")), (Some("gzip"), Some(nfsu2_headers::EDGE_KEEP)));
    assert_eq!(decoded("gzip", &a.body), block(1));
    let a = kit.get(&format!("/data-blocks/v2/TRACKS/late.bin?b=0&n=2&{v}"), &all).await;
    assert_eq!(a.header("cloudflare-cdn-cache-control"), Some(nfsu2_headers::EDGE_KEEP));
    assert_eq!(decoded(a.header("content-encoding").unwrap_or(""), &a.body), raw[..2 << 20]);
    // the 5-minute line's counts, then none
    let line = kit.app.sent.take();
    assert!(
        line.starts_with(|c: char| c.is_ascii_digit() && c != '0') && line.contains(" kept, ") && line.contains(" as they are"),
        "{line}"
    );
    assert_eq!(kit.app.sent.take(), "");
}

// The manifest's files: every block kept without being asked for (the sweep at the start)
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn the_manifests_blocks_are_kept_unasked() {
    let kit = Kit::start_keeping(2, |root| std::fs::write(root.join("game/TRACKS/swept.bin"), data((2 << 20) + 5000, true)).unwrap()).await;
    let raw = std::fs::read(kit.path("game/TRACKS/swept.bin")).unwrap();
    let v = version(&kit, "TRACKS/swept.bin");
    // swept.bin's three blocks kept: a HEAD of a kept one is br (of one not kept, the head of the block as it is), and
    // queues none.  (Counting the files in blocks/ took the half-written ones too, and the manifest's other blocks: it
    // went on before these were all kept.)
    let t0 = std::time::Instant::now();
    for b in 0..3usize {
        let path = format!("/data-blocks/v2/TRACKS/swept.bin?b={b}&n=1&{v}");
        while kit.request("HEAD", &path, &[("Accept-Encoding", "br")], b"").await.header("content-encoding") != Some("br") {
            assert!(t0.elapsed() < Duration::from_secs(30), "the manifest's block {b} kept in 30 s");
            tokio::time::sleep(Duration::from_millis(50)).await;
        }
    }
    for b in 0..3usize {
        let a = kit.get(&format!("/data-blocks/v2/TRACKS/swept.bin?b={b}&n=1&{v}"), &[("Accept-Encoding", "br")]).await;
        assert_eq!(a.header("content-encoding"), Some("br"), "block {b}");
        assert!(a.header("content-length").is_some());
        assert_eq!(decoded("br", &a.body), raw[b << 20..raw.len().min((b + 1) << 20)], "block {b}");
    }
}

// A file put in place while it is asked for (a build or a deploy renames the new version over the old one): every
// answer is one version, with that version's length and ETag.  (The size and ETag came from a look at the path, the
// bytes from an open after it: an answer with one version's length and the other's bytes, cut short or cut off.)
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_file_put_in_place_meanwhile_is_answered_as_one_version() {
    let kit = Kit::start(&[], |root| std::fs::write(root.join("game/GLOBAL/x.bin"), vec![b'a'; 10_000]).unwrap()).await;
    let (path, tmp) = (kit.path("game/GLOBAL/x.bin"), kit.path("game/x.tmp"));
    let stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let writer = {
        let stop = stop.clone();
        std::thread::spawn(move || {
            let mut long = true;
            while !stop.load(std::sync::atomic::Ordering::Relaxed) {
                let (byte, n) = if long { (b'b', 20_000) } else { (b'a', 10_000) };
                std::fs::write(&tmp, vec![byte; n]).unwrap();
                std::fs::rename(&tmp, &path).unwrap();
                long = !long;
            }
        })
    };
    let mut seen: std::collections::HashMap<String, (u8, usize)> = std::collections::HashMap::new();
    let (mut bad, mut n) = (Vec::new(), 0);
    let t0 = std::time::Instant::now();
    while t0.elapsed() < Duration::from_millis(1500) {
        n += 1;
        let mut s = kit.send("/data/GLOBAL/x.bin", &[]).await;
        let mut b = Vec::new();
        if s.read_to_end(&mut b).await.is_err() || !b.windows(4).any(|w| w == b"\r\n\r\n") {
            bad.push("the connection broke".to_string());
            continue;
        }
        let a = super::testkit::Answer::parse(&b);
        let len: usize = a.header("content-length").and_then(|v| v.parse().ok()).unwrap_or(usize::MAX);
        let etag = a.header("etag").unwrap_or("").to_string();
        let one = a.body.first().copied().unwrap_or(0);
        if a.status != 200 || a.body.len() != len || a.body.iter().any(|&c| c != one) || len != if one == b'a' { 10_000 } else { 20_000 } {
            bad.push(format!("{} {} bytes of {:?} as {len}", a.status, a.body.len(), one as char));
        } else if *seen.entry(etag).or_insert((one, len)) != (one, len) {
            bad.push("one ETag for two versions".to_string());
        }
    }
    stop.store(true, std::sync::atomic::Ordering::Relaxed);
    writer.join().unwrap();
    assert!(bad.is_empty(), "{} of {n} answers: {:?}", bad.len(), &bad[..bad.len().min(5)]);
}

/// the status of a WebSocket upgrade to `path` on the name `host` (Origin: that name's page, when given)
async fn upgrade_status(kit: &Kit, host: &str, path: &str, origin: Option<&str>) -> u16 {
    let mut s = tokio::net::TcpStream::connect(kit.addr).await.unwrap();
    let origin = origin.map_or(String::new(), |o| format!("Origin: {o}\r\n"));
    let q = format!(
        "GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n{origin}\
         Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n"
    );
    s.write_all(q.as_bytes()).await.unwrap();
    let mut b = [0u8; 64];
    let n = tokio::time::timeout(Duration::from_secs(5), s.read(&mut b)).await.expect("an answer in time").unwrap();
    String::from_utf8_lossy(&b[..n]).get(9..12).and_then(|c| c.parse().ok()).unwrap_or(0)
}

/// a module and its json where the game's defaults look (`wasm`: the module's path in the repo)
fn module(root: &std::path::Path, wasm: &str) {
    let p = root.join("repo").join(wasm);
    std::fs::create_dir_all(p.parent().unwrap()).unwrap();
    std::fs::write(&p, data(5000, true)).unwrap();
    std::fs::write(p.with_extension("json"), br#"{"rev":"abc"}"#).unwrap();
}

// The routes per base (PLAN D-38): UG2 at the root on its own name and this machine's, as ever; under /u2 on the
// site it shares with Most Wanted: there "/u2" goes to "/u2/", no other path answers but the root files (robots.txt
// naming both games' bases), and the manifest names its blocks under /u2, with an ETag of its own.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn ug2_under_u2_on_the_shared_site_and_at_the_root_elsewhere() {
    let kit = Kit::start(&["--host", "nfsu2.xyz,nfsrevived.xyz"], |root| {
        module(root, "out/web/nfsu2.wasm");
        std::fs::write(root.join("repo/web/dist/main.js"), b"// the page").unwrap();
    })
    .await;
    let get = |host: &'static str, path: &'static str| kit.request_to(host, "GET", path, &[], b"");
    let shared = "nfsrevived.xyz";
    // under /u2: the page (the one indexable answer), the module, the game's files, the players
    let a = get(shared, "/u2/").await;
    assert_eq!((a.status, a.body.as_slice(), a.header("x-robots-tag")), (200, &b"<!doctype html>"[..], Some("index, follow")));
    for p in ["/u2/index.html", "/u2/main.js", "/u2/game.json", "/u2/game.wasm", "/u2/players", "/u2/data/SPEED2.EXE", "/u2/data-manifest"]
    {
        let a = get(shared, p).await;
        assert_eq!(a.status, 200, "{p}");
        let want = if p == "/u2/index.html" { "index, follow" } else { "noindex, nofollow" };
        assert_eq!(a.header("x-robots-tag"), Some(want), "{p}");
    }
    assert_eq!(get(shared, "/u2/data/SPEED2.EXE").await.body, b"MZ the game");
    let a = get(shared, "/u2/data-blocks/v2/GLOBAL/small.bin?b=0&n=1").await;
    assert_eq!((a.status, a.body.len()), (200, 1000));
    // the base itself: 308 to its folder, the query kept; for GET and HEAD only
    let a = get(shared, "/u2").await;
    assert_eq!((a.status, a.header("location"), a.header("x-robots-tag")), (308, Some("/u2/"), Some("noindex, nofollow")));
    assert_eq!(get(shared, "/u2?bc=0&a=%C3%A9").await.header("location"), Some("/u2/?bc=0&a=%C3%A9"));
    let h = kit.request_to(shared, "HEAD", "/u2", &[], b"").await;
    assert_eq!((h.status, h.header("location"), h.body.len()), (308, Some("/u2/"), 0));
    assert_eq!(kit.request_to(shared, "PUT", "/u2", &[], b"x").await.status, 405);
    // the rest of that origin: no route of UG2's (other spellings are the edge's to redirect)
    for p in [
        "/",
        "/index.html",
        "/main.js",
        "/game.json",
        "/game.wasm",
        "/players",
        "/data-manifest",
        "/data/SPEED2.EXE",
        "/data-blocks/v2/GLOBAL/small.bin?b=0&n=1",
        "/vault",
        "/names",
        "/u2x/",
        "/u2.html",
        "/U2/",
        "/U2",
        "/mw/",
        "/mw",
        "/u2/../game.json",
        "/u2/%2e%2e/game.json",
        "/u%32/x/../../game.json",
    ] {
        assert_eq!(get(shared, p).await.status, 404, "{p}");
    }
    assert_eq!(kit.request_to(shared, "PUT", "/vault", &[], b"x").await.status, 405);
    // robots.txt: at the root only, naming both games' bases
    assert_eq!(get(shared, "/u2/robots.txt").await.status, 404);
    let r = get(shared, "/robots.txt").await;
    assert_eq!(
        (r.status, r.header("x-robots-tag"), r.header("content-type")),
        (200, Some("index, follow"), Some("text/plain; charset=utf-8"))
    );
    let want = "User-agent: *\nAllow: /\nDisallow: /u2/data/\nDisallow: /u2/data-manifest\nDisallow: /u2/game.wasm\nDisallow: /u2/game.json\n\
                Disallow: /u2/players\nDisallow: /u2/lan\nDisallow: /u2/vault\nDisallow: /u2/names\nDisallow: /mw/data/\n\
                Disallow: /mw/data-manifest\nDisallow: /mw/game.wasm\nDisallow: /mw/game.json\nDisallow: /mw/players\nDisallow: /mw/lan\n\
                Disallow: /mw/vault\nDisallow: /mw/names\n";
    assert_eq!(String::from_utf8_lossy(&r.body), want);
    // keyphrase saves under /u2 (no token: 401, how a page tells a server with them)
    assert_eq!(get(shared, "/u2/vault").await.status, 401);
    // /lan: under /u2 there, at the root here
    assert_eq!(upgrade_status(&kit, shared, "/u2/lan", Some("https://nfsrevived.xyz")).await, 101);
    assert_eq!(upgrade_status(&kit, shared, "/lan", None).await, 404);
    assert_eq!(upgrade_status(&kit, "localhost", "/u2/lan", None).await, 404);
    assert_eq!(upgrade_status(&kit, "nfsu2.xyz", "/lan", Some("https://nfsu2.xyz")).await, 101);
    // the old site and this machine: everything at the root, as ever, and nothing under /u2
    for host in ["nfsu2.xyz", "localhost", "127.0.0.1"] {
        let r = kit.request_to(host, "GET", "/robots.txt", &[], b"").await;
        assert_eq!((r.status, String::from_utf8_lossy(&r.body)), (200, format!("{}\n", super::ROBOTS_TXT).into()), "{host}");
        for p in ["/", "/game.json", "/players", "/data/SPEED2.EXE", "/data-manifest"] {
            assert_eq!(kit.request_to(host, "GET", p, &[], b"").await.status, 200, "{host} {p}");
        }
        for p in ["/u2", "/u2/", "/u2/game.json"] {
            assert_eq!(kit.request_to(host, "GET", p, &[], b"").await.status, 404, "{host} {p}");
        }
    }
    // the manifest: the same files; the blocks under the base; one ETag per base, the root's the manifest's of before
    let (root, u2) = (get("nfsu2.xyz", "/data-manifest").await, get(shared, "/u2/data-manifest").await);
    let (rv, uv): (serde_json::Value, serde_json::Value) =
        (serde_json::from_slice(&root.body).unwrap(), serde_json::from_slice(&u2.body).unwrap());
    assert_eq!(rv["files"], uv["files"]);
    assert_eq!((rv["blocks"]["url"].as_str(), uv["blocks"]["url"].as_str()), (Some("/data-blocks/v2/"), Some("/u2/data-blocks/v2/")));
    let (re, ue) = (root.header("etag").unwrap(), u2.header("etag").unwrap());
    assert_ne!(re, ue);
    let local = get("localhost", "/data-manifest").await;
    assert_eq!((&local.body, local.header("etag")), (&root.body, Some(re)));
    let key = super::etag::load_key(&kit.path("state").to_string_lossy()).unwrap();
    let before =
        super::manifest::build(&crate::game::UG2, &[String::new()], &kit.path("game").to_string_lossy(), "/nonexistent-state", &key)
            .unwrap();
    assert_eq!((&root.body[..], re), (&before.bodies[0].body[..], before.bodies[0].etag.as_str()));
    let not_modified = kit.request_to(shared, "GET", "/u2/data-manifest", &[("If-None-Match", ue)], b"").await;
    assert_eq!(not_modified.status, 304);
    assert_eq!(kit.request_to(shared, "GET", "/u2/data-manifest", &[("If-None-Match", re)], b"").await.status, 200);
    let gz = kit.request_to(shared, "GET", "/u2/data-manifest?refresh", &[("Accept-Encoding", "gzip")], b"").await;
    assert_eq!((gz.status, gz.header("content-encoding"), gz.header("etag")), (200, Some("gzip"), Some(ue)));
    let mut un = Vec::new();
    std::io::Read::read_to_end(&mut flate2::read::GzDecoder::new(&gz.body[..]), &mut un).unwrap();
    assert_eq!(un, u2.body);
}

// Random targets on the shared site, spelled every way the URL parser reads (case, escapes, dot segments, slashes):
// an answer only under /u2/ (the vault's 401 at /u2/vault), the root's robots.txt, and /u2 itself to /u2/; nothing
// else but 400 and 404, whatever the spelling
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn nothing_outside_the_base_answers() {
    let kit = Kit::start(&["--host", "nfsrevived.xyz"], |root| module(root, "out/web/nfsu2.wasm")).await;
    let words: Vec<&str> =
        r"/ u2 U2 u 2 %32 %75 %2e %2E . .. %2f %5c \ ? x index.html game.json robots.txt data -manifest lan vault players % %00 // ; mw &"
            .split(' ')
            .collect();
    let mut x = 0x9e37_79b9_7f4a_7c15u64;
    let mut next = move || {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        x
    };
    let mut seen = std::collections::BTreeMap::new();
    for _ in 0..2000 {
        let mut t = String::from("/");
        for _ in 0..1 + next() % 6 {
            t.push_str(words[(next() % words.len() as u64) as usize]);
        }
        let a = kit.request_to("nfsrevived.xyz", "GET", &t, &[], b"").await;
        // the path the routes read: the URL parser's, decoded
        let p = super::url::decode_uri_component(&super::url::parse_path(&t).pathname);
        let p = p.as_deref().unwrap_or("(none)");
        match a.status {
            200 => assert!(p.starts_with("/u2/") || p == "/robots.txt", "{t:?}: 200 for {p:?}"),
            401 => assert_eq!(p, "/u2/vault", "{t:?}"),
            308 => assert_eq!((p, a.header("location").map(|l| l.starts_with("/u2/"))), ("/u2", Some(true)), "{t:?}"),
            400 | 404 => {}
            s => panic!("{t:?}: {s} for {p:?}"),
        }
        *seen.entry(a.status).or_insert(0) += 1;
    }
    assert!([200, 308, 400, 404].iter().all(|s| seen.contains_key(s)), "every kind of answer seen: {seen:?}");
}

// --base-path: this machine's name stands in for the shared site (the tests' one-origin layouts), its root files and
// all; "*" moves every other name
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn base_path_moves_a_name() {
    let kit = Kit::start(&["--base-path", "localhost=/u2,*=/x"], |_| {}).await;
    let get = |host: &'static str, path: &'static str| kit.request_to(host, "GET", path, &[], b"");
    assert_eq!(get("localhost:9", "/u2/").await.status, 200);
    assert_eq!(get("localhost", "/").await.status, 404);
    let r = get("localhost", "/robots.txt").await;
    assert!(String::from_utf8_lossy(&r.body).starts_with("User-agent: *\nAllow: /\nDisallow: /u2/data/\n"));
    assert!(String::from_utf8_lossy(&r.body).ends_with("Disallow: /mw/vault\nDisallow: /mw/names\n"));
    assert_eq!(get("127.0.0.1", "/x/").await.status, 200);
    assert_eq!(get("127.0.0.1", "/x").await.header("location"), Some("/x/"));
    assert_eq!(get("127.0.0.1", "/u2/").await.status, 404);
    let r = get("127.0.0.1", "/robots.txt").await;
    assert!(String::from_utf8_lossy(&r.body).starts_with("User-agent: *\nAllow: /\nDisallow: /x/data/\n"));
    let m: serde_json::Value = serde_json::from_slice(&get("127.0.0.1", "/x/data-manifest").await.body).unwrap();
    assert_eq!(m["blocks"]["url"], "/x/data-blocks/v2/");
    let m: serde_json::Value = serde_json::from_slice(&get("localhost", "/u2/data-manifest").await.body).unwrap();
    assert_eq!(m["blocks"]["url"], "/u2/data-blocks/v2/");
    // (a body for each base of its names: /x, and localhost's /u2; no name is at the root.  The shared site's name is
    // none of its own, no --host, so its /u2 alone would make none)
    let bases: Vec<String> = kit.app.current_manifest().unwrap().bodies.iter().map(|b| b.base.clone()).collect();
    assert_eq!(bases, ["/x", "/u2"]);
}

/// the bases the manifest that is in place has a body for
fn bodies(kit: &Kit) -> Vec<String> {
    kit.app.current_manifest().unwrap().bodies.iter().map(|b| b.base.clone()).collect()
}

// A manifest body only for the bases of the server's own names: UG2's server for nfsu2.xyz alone makes none for the
// shared site's /u2, where it answers 421.  A name it takes on later (a new certificate's) gets its body at its first
// request, from a walk then; the root's body and ETag stay as they were.
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn a_manifest_body_only_for_the_bases_of_its_names() {
    let kit = Kit::start(&["--host", "nfsu2.xyz"], |_| {}).await;
    let get = |host: &'static str, path: &'static str| kit.request_to(host, "GET", path, &[], b"");
    assert_eq!(bodies(&kit), [""]);
    let root = get("nfsu2.xyz", "/data-manifest").await;
    assert_eq!(get("nfsrevived.xyz", "/u2/data-manifest").await.status, 421);
    let walked = kit.app.current_manifest().unwrap();
    assert_eq!(bodies(&kit), [""], "no walk for a name it does not answer");
    kit.app.host_check.set_cert_names(&["nfsrevived.xyz".to_string()]);
    let u2 = get("nfsrevived.xyz", "/u2/data-manifest").await;
    assert_eq!(u2.status, 200);
    let m: serde_json::Value = serde_json::from_slice(&u2.body).unwrap();
    assert_eq!(m["blocks"]["url"], "/u2/data-blocks/v2/");
    assert!(!Arc::ptr_eq(&walked, &kit.app.current_manifest().unwrap()), "walked again");
    assert_eq!(bodies(&kit), ["", "/u2"]);
    let again = get("nfsu2.xyz", "/data-manifest").await;
    assert_eq!((&again.body, again.header("etag")), (&root.body, root.header("etag")));
    assert_ne!(u2.header("etag"), root.header("etag"));
    // and no walk for each request after it
    let now = kit.app.current_manifest().unwrap();
    assert_eq!(get("nfsrevived.xyz", "/u2/data-manifest").await.header("etag"), u2.header("etag"));
    assert!(Arc::ptr_eq(&now, &kit.app.current_manifest().unwrap()));
}

// The launch switch (game.rs Handover): off, as in every build until the launch release, every answer's
// Content-Security-Policy is the one of before on every name.  On, the old address's answers (Host nfsu2.xyz) let its
// pages connect to the new address and those of its loopback stand-in (127.0.0.1) to localhost's /u2/, while the
// new address's, localhost's and Most Wanted's (which has no old address) stay as they are, byte for byte
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn the_launch_switch_lets_only_the_old_addresss_pages_connect_to_the_new_one() {
    let csp = nfsu2_headers::CSP;
    let ug2 = |root: &std::path::Path| {
        module(root, "out/web/nfsu2.wasm");
        std::fs::write(root.join("repo/web/dist/main.js"), b"// the page").unwrap();
    };
    let args = ["--host", "nfsu2.xyz,nfsrevived.xyz"];
    let paths = |host: &str| -> [&'static str; 5] {
        if host == "nfsrevived.xyz" {
            ["/u2/", "/u2/main.js", "/u2/game.json", "/u2/vault", "/u2/none"]
        } else {
            ["/", "/main.js", "/game.json", "/vault", "/none"]
        }
    };
    let csp_of = |a: &super::testkit::Answer| a.header("content-security-policy").map(str::to_string);
    let off = Kit::start(&args, ug2).await;
    for host in ["nfsu2.xyz", "nfsrevived.xyz", "localhost", "127.0.0.1:8080"] {
        for p in paths(host) {
            let a = off.request_to(host, "GET", p, &[], b"").await;
            assert_eq!(csp_of(&a).as_deref(), Some(csp), "{host} {p}: {}", a.status);
        }
    }
    let on = Kit::start_launched(&args, ug2).await;
    for (host, want) in [
        ("nfsu2.xyz", csp.replace("connect-src 'self';", "connect-src 'self' https://nfsrevived.xyz/u2/;")),
        ("127.0.0.1:8080", csp.replace("connect-src 'self';", "connect-src 'self' http://localhost:*/u2/;")),
        ("nfsrevived.xyz", csp.to_string()),
        ("localhost", csp.to_string()),
    ] {
        for p in paths(host) {
            let a = on.request_to(host, "GET", p, &[], b"").await;
            assert_eq!(csp_of(&a).as_deref(), Some(want.as_str()), "{host} {p}: {}", a.status);
        }
    }
    let mw = Kit::start_launched(&["--game", "mw", "--host", "nfsu2.xyz"], |root| {
        std::fs::write(root.join("game/speed.exe"), b"MZ most wanted").unwrap();
        std::fs::create_dir_all(root.join("repo/web/dist-mw")).unwrap();
        std::fs::write(root.join("repo/web/dist-mw/index.html"), b"<!doctype html>").unwrap();
        module(root, "out/web/mw/nfsmw.wasm");
    })
    .await;
    for host in ["nfsu2.xyz", "127.0.0.1"] {
        let a = mw.request_to(host, "GET", "/mw/", &[], b"").await;
        assert_eq!((a.status, csp_of(&a).as_deref()), (200, Some(csp)), "{host}");
    }
}

// Most Wanted: under /mw on every name, this machine's too; no root files of its own (UG2's server answers the shared
// site's root)
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn most_wanted_under_mw_everywhere() {
    let kit = Kit::start(&["--game", "mw"], |root| {
        std::fs::write(root.join("game/speed.exe"), b"MZ most wanted").unwrap();
        std::fs::create_dir_all(root.join("repo/web/dist-mw")).unwrap();
        std::fs::write(root.join("repo/web/dist-mw/index.html"), b"<!doctype html><title>mw</title>").unwrap();
        module(root, "out/web/mw/nfsmw.wasm");
    })
    .await;
    for host in ["localhost", "127.0.0.1:8080"] {
        let get = |path: &'static str| kit.request_to(host, "GET", path, &[], b"");
        let a = get("/mw/").await;
        assert_eq!((a.status, a.header("x-robots-tag")), (200, Some("index, follow")), "{host}");
        assert_eq!(a.body, b"<!doctype html><title>mw</title>");
        for p in ["/mw/game.json", "/mw/game.wasm", "/mw/players", "/mw/data/speed.exe", "/mw/data-manifest"] {
            assert_eq!(get(p).await.status, 200, "{host} {p}");
        }
        assert_eq!(get("/mw/vault").await.status, 401, "{host}");
        assert_eq!(get("/mw").await.header("location"), Some("/mw/"), "{host}");
        for p in
            ["/", "/robots.txt", "/mw/robots.txt", "/game.json", "/players", "/data-manifest", "/data/speed.exe", "/u2/", "/MW/", "/mwx"]
        {
            assert_eq!(get(p).await.status, 404, "{host} {p}");
        }
        let m: serde_json::Value = serde_json::from_slice(&get("/mw/data-manifest").await.body).unwrap();
        assert_eq!(m["blocks"]["url"], "/mw/data-blocks/v2/");
        assert_eq!(upgrade_status(&kit, host, "/mw/lan", None).await, 101, "{host}");
        assert_eq!(upgrade_status(&kit, host, "/lan", None).await, 404, "{host}");
    }
    // a name it does not have: 421 before any path
    assert_eq!(kit.request_to("nfsrevived.xyz", "GET", "/mw/", &[], b"").await.status, 421);
}

/// an answer's whole length once its head is in (its Content-Length; none: no body)
fn answer_len(b: &[u8]) -> Option<usize> {
    let end = b.windows(4).position(|w| w == b"\r\n\r\n")?;
    let head = String::from_utf8_lossy(&b[..end]).to_ascii_lowercase();
    let len = head.split("content-length: ").nth(1).and_then(|r| r.split("\r\n").next()).and_then(|v| v.parse().ok()).unwrap_or(0);
    Some(end + 4 + len)
}

/// A measurement, not a check: requests a second and the server's CPU per request for the small routes (the
/// router, the headers, the file checks), 32 keep-alive connections, one request at a time on each, the client on
/// a runtime of its own.  cargo test --release -- --ignored bench_routes --nocapture
#[test]
#[ignore]
fn bench_routes() {
    let server = tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build().unwrap();
    let kit = server.block_on(Kit::start(&[], |_| {}));
    let etag = server.block_on(kit.get("/index.html", &[])).header("etag").unwrap().to_string();
    let addr = kit.addr;
    let client = tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build().unwrap();
    for (name, path, extra) in [
        ("/players", "/players", String::new()),
        ("/index.html 200", "/index.html", String::new()),
        ("/index.html 304", "/index.html", format!("If-None-Match: {etag}\r\n")),
        ("/data/ range", "/data/GLOBAL/small.bin", "Range: bytes=0-99\r\n".to_string()),
        ("/data-blocks/ as is", "/data-blocks/GLOBAL/small.bin?b=0&n=1", String::new()),
        ("404", "/nothing/here.js", String::new()),
    ] {
        // (NFSU2_BENCH_ROUTE: that one alone, for a profiler)
        if std::env::var("NFSU2_BENCH_ROUTE").is_ok_and(|r| r != name) {
            continue;
        }
        let (conns, per) = (32, 2000);
        let req = format!("GET {path} HTTP/1.1\r\nHost: localhost\r\nUser-Agent: bench\r\n{extra}\r\n");
        let cpu0 = nfsu2_util::cpu::process_cpu_seconds();
        let t0 = std::time::Instant::now();
        client.block_on(async {
            let tasks: Vec<_> = (0..conns)
                .map(|_| {
                    let req = req.clone();
                    tokio::spawn(async move {
                        let mut s = tokio::net::TcpStream::connect(addr).await.unwrap();
                        s.set_nodelay(true).unwrap();
                        let mut buf = vec![0u8; 64 << 10];
                        for _ in 0..per {
                            s.write_all(req.as_bytes()).await.unwrap();
                            let (mut got, mut want) = (0, usize::MAX);
                            while got < want {
                                let n = s.read(&mut buf[got..]).await.unwrap();
                                assert!(n > 0, "closed");
                                got += n;
                                if want == usize::MAX {
                                    want = answer_len(&buf[..got]).unwrap_or(usize::MAX);
                                }
                            }
                        }
                    })
                })
                .collect();
            for t in tasks {
                t.await.unwrap();
            }
        });
        let s = t0.elapsed().as_secs_f64();
        // (the process's CPU: the client's runtime too, which costs about the same for every route)
        let cpu = nfsu2_util::cpu::process_cpu_seconds() - cpu0;
        let n = (conns * per) as f64;
        println!("{name:<22} {:>8.0} requests/s  {:>6.1} us of CPU each (server and client)", n / s, cpu / n * 1e6);
    }
    drop(kit);
}
