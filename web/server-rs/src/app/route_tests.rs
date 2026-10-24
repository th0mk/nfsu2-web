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

// /rankings: each race type's ranking from the Online host's stats.json, best first, overall and this month (a
// persona's counts of an earlier month are not this month's), the fields the game shows and no others; nothing with
// --online off
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn rankings_come_from_the_online_results() {
    let month = &nfsu2_util::js::iso_string(nfsu2_util::now_ms() as i64)[..7];
    let stats = format!(
        r#"{{"personas":[
        {{"name":"Alpha","seen":"{month}","all":{{"0":{{"races":3,"rw":2,"rl":1,"points":200}}}},"month":{{"key":"{month}","t":{{"0":{{"races":1,"rw":1,"points":100}}}}}}}},
        {{"name":"Bravo","seen":"{month}","all":{{"0":{{"races":4,"rw":1,"rl":3,"points":250}},"1":{{"races":1,"ul":1}}}},"month":{{"key":"{month}","t":{{"1":{{"races":1,"ul":1}}}}}}}},
        {{"name":"Charlie","seen":"2026-01","all":{{"0":{{"races":1,"rl":1}}}},"month":{{"key":"2026-01","t":{{"0":{{"races":5,"rw":5,"points":500}}}}}}}}]}}"#
    );
    let kit = Kit::start(&["--online", "on"], |root| {
        std::fs::create_dir_all(root.join("state/online")).unwrap();
        std::fs::write(root.join("state/online/stats.json"), &stats).unwrap();
    })
    .await;
    let a = kit.get("/rankings", &[]).await;
    assert_eq!((a.status, a.header("content-type")), (200, Some("application/json")));
    let v: serde_json::Value = serde_json::from_slice(&a.body).unwrap();
    let rows = |k: &str, t: &str| serde_json::to_string(&v[k][t]).unwrap();
    assert_eq!(
        rows("all", "circuit"),
        r#"[{"rank":1,"name":"Bravo","points":250,"wins":1,"losses":3},{"rank":2,"name":"Alpha","points":200,"wins":2,"losses":1},{"rank":3,"name":"Charlie","points":0,"wins":0,"losses":1}]"#
    );
    assert_eq!(rows("month", "circuit"), r#"[{"rank":1,"name":"Alpha","points":100,"wins":1,"losses":0}]"#);
    assert_eq!(rows("month", "sprint"), r#"[{"rank":1,"name":"Bravo","points":0,"wins":0,"losses":1}]"#);
    let keys: Vec<&String> = v["all"].as_object().unwrap().keys().collect();
    assert_eq!(keys, ["circuit", "sprint", "drag", "drift", "streetx", "url"]);
    assert_eq!(v["month"]["drag"], serde_json::json!([]));
    let off = Kit::start(&[], |_| {}).await;
    assert_eq!(off.get("/rankings", &[]).await.status, 404);
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
