//! The switch's own tests, the first half (tests/lan_rules.rs has the second), on a switch on an ephemeral port with
//! fake browsers (tests/harness).  The real server's /lan (its Origin check, ?resume) is web/test/lan.test.mjs's; the
//! page's own net code runs in node against this switch in tests/page.rs.
//!
//! Address assignment, peer lists, unicast and broadcast routing (source rewritten), UDP frames passed through
//! untouched, ping/pong, disconnects, reconnecting to the same address (only with the token the welcome gave: the
//! audit's F1), the latency of a frame through the switch on this machine, what a peer learns about the others,
//! the peer list, the limits (players, frame size and rate, control messages, a broadcast charged per receiver,
//! limits shared by one client address, a flood never getting a slower receiver cut: the audit's F2), WebSocket
//! keepalive and idle close, the 6000 addresses of 10.77.0.0/19, LAN play's broadcasts only to the browsers with it
//! open, a page that lets go freeing its address at once, bundles.

mod harness;

use std::collections::HashSet;
use std::time::Instant;

use harness::*;
use nfsu2_lan::LanOptions;
use serde_json::Value;
use tokio::io::{AsyncReadExt, AsyncWriteExt};

fn vip_of(m: &Value) -> u32 {
    m["vip"].as_u64().unwrap() as u32
}

// addresses, peer lists, unicast, broadcast, ping, disconnect, reconnect
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn addresses_peer_lists_unicast_broadcast_ping_disconnect_reconnect() {
    let _s = serial().await;
    let sw = start(opts()).await;
    // (one after another: the addresses follow the order of arrival; opened all at once, which gets which is a race)
    let mut a = Client::connect(sw.port, "Alice").await;
    let wa = a.welcome().await;
    let mut b = Client::connect(sw.port, "Bob").await;
    let wb = b.welcome().await;
    let mut c = Client::connect(sw.port, "Carol").await;
    let wc = c.welcome().await;
    // addresses: round the twenty /24s of 10.77.0.0/19 in order of arrival; mask /19
    assert_eq!(wa["vipStr"], "10.77.0.2");
    assert_eq!(wb["vipStr"], "10.77.1.2");
    assert_eq!(wc["vipStr"], "10.77.2.2");
    assert_eq!(ip_str(vip_of(&wa)), "10.77.0.2");
    assert_eq!(wa["mask"], "255.255.224.0");
    // everyone's peer list ends with all three names
    for x in [&a, &b, &c] {
        x.until_true(|g| g.names() == "Alice,Bob,Carol").await;
    }

    // unicast A -> B, with a forged source: B sees A's real address, ports and data intact; C sees nothing
    a.udp(b.vip, 47210, 47211, b"hello bob", 1, ip("10.77.0.99"));
    let fb = b.until(|g| g.frames.first().map(|f| f.0.clone())).await;
    assert_eq!(fb[0], 1);
    assert_eq!(u32::from_be_bytes(fb[4..8].try_into().unwrap()), a.vip, "source rewritten to the sender");
    assert_eq!(u32::from_be_bytes(fb[8..12].try_into().unwrap()), b.vip);
    assert_eq!(u16::from_be_bytes([fb[12], fb[13]]), 47210);
    assert_eq!(u16::from_be_bytes([fb[14], fb[15]]), 47211);
    assert_eq!(&fb[16..], b"hello bob");
    sleep(50).await;
    assert_eq!(c.frames(), 0, "unicast reaches only its destination");
    assert_eq!(a.frames(), 0);

    // broadcast from C: A and B get it, C does not; kind 2 (test loopback) is routed like any kind
    c.udp(BROADCAST, 47300, 47301, b"beacon", 2, 0);
    let ga = a.until(|g| g.frames.first().map(|f| f.0.clone())).await;
    let gb = b.until(|g| g.frames.get(1).map(|f| f.0.clone())).await;
    for g in [ga, gb] {
        assert_eq!(g[0], 2);
        assert_eq!(u32::from_be_bytes(g[4..8].try_into().unwrap()), c.vip);
        assert_eq!(u32::from_be_bytes(g[8..12].try_into().unwrap()), BROADCAST);
        assert_eq!(u16::from_be_bytes([g[14], g[15]]), 47301);
        assert_eq!(&g[16..], b"beacon");
    }
    sleep(50).await;
    assert_eq!(c.frames(), 0, "a broadcast does not come back to its sender");

    // a frame shorter than the switch header is ignored
    a.binary(vec![0; 8]);
    // ping / pong
    a.text(r#"{"t":"ping","id":7}"#);
    a.until_true(|g| g.ctl.iter().any(|m| m["t"] == "pong" && m["id"] == 7)).await;

    // B leaves: the others' peer lists drop it; frames to its address are counted as dropped
    let bvip = b.vip;
    b.close().await;
    a.until_true(|g| g.last_peers().is_some_and(|p| p.len() == 2) && !g.names().contains("Bob")).await;
    let d0 = sw.info().await["dropped"].as_u64().unwrap();
    a.udp(bvip, 1, 2, b"into the void", 1, 0);
    sleep(50).await;
    assert_eq!(sw.info().await["dropped"].as_u64().unwrap(), d0 + 1);

    // B comes back with its token (?resume, then the hello): it gets its address back
    let wbt = wb["token"].as_str().unwrap();
    let mut b2 = Client::new(sw.port, "Bob", "", Some((bvip, wbt)), Opts::default()).await;
    let wb2 = b2.welcome().await;
    assert_eq!(vip_of(&wb2), bvip, "a reconnect with the token gets the same address back");
    assert_ne!(wb2["token"], wb["token"], "a new token each welcome");
    // the old request (?vip=), a taken address, a wrong token, or one outside the LAN: a fresh address
    let x22 = "x".repeat(22);
    let mut d = Client::new(sw.port, "Dave", &format!("?vip={}", ip_str(a.vip)), Some((a.vip, &x22)), Opts::default()).await;
    let wd = d.welcome().await;
    assert_ne!(vip_of(&wd), a.vip);
    let wat = wa["token"].as_str().unwrap();
    let mut e = Client::new(sw.port, "Eve", "?vip=192.168.1.20", Some((ip("192.168.1.20"), wat)), Opts::default()).await;
    let we = e.welcome().await;
    assert_eq!(vip_of(&we) >> 24, 10);
    assert_eq!((vip_of(&we) >> 16) & 255, 77);
    let mut f = Client::with(sw.port, "Fred", "?vip=10.77.0.255", Opts::default()).await;
    let wf = f.welcome().await;
    assert!((2..=251).contains(&(vip_of(&wf) & 255)), "only host addresses .2 .. .251");
    // all addresses distinct
    let vips: HashSet<u32> = [a.vip, b2.vip, c.vip, d.vip, e.vip, f.vip].into_iter().collect();
    assert_eq!(vips.len(), 6);
    // A -> the reconnected B works again
    a.udp(bvip, 5, 6, b"welcome back", 1, 0);
    let fb2 = b2.until(|g| g.frames.first().map(|f| f.0.clone())).await;
    assert_eq!(&fb2[16..], b"welcome back");

    assert_eq!(sw.info().await["peers"].as_array().unwrap().len(), 6);
    for x in [&a, &b2, &c, &d, &e, &f] {
        x.close().await;
    }
    sleep(50).await;
    assert_eq!(sw.info().await["peers"].as_array().unwrap().len(), 0, "every address freed");
    sw.close().await;
}

// latency of a frame through the switch (this machine)
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn latency_of_a_frame_through_the_switch() {
    let _s = serial().await;
    // limits far above the 2160 frames sent here (the limits have tests of their own)
    let sw = start(LanOptions { frame_rate: 100000.0, group_frame_rate: 100000.0, ..opts() }).await;
    let mut a = Client::connect(sw.port, "A").await;
    a.welcome().await;
    let mut b = Client::connect(sw.port, "B").await;
    b.welcome().await;
    const N: usize = 2000;
    let mut lat = Vec::with_capacity(N);
    // 200-byte datagrams (a roam snapshot is 182 bytes), one at a time
    for i in 0..N {
        let t0 = Instant::now();
        a.udp(b.vip, 47210, 47210, &[i as u8; 200], 1, 0);
        let t = b.until(|g| g.frames.get(i).map(|f| f.1)).await;
        lat.push(t.duration_since(t0).as_secs_f64() * 1000.0);
    }
    lat.sort_by(f64::total_cmp);
    println!(
        "switch one-way latency, {N} frames of 216 bytes over localhost: p50 {:.3} ms, p99 {:.3} ms, max {:.3} ms",
        q(&lat, 0.5),
        q(&lat, 0.99),
        lat[N - 1]
    );
    assert!(q(&lat, 0.5) < 5.0, "well under a frame");
    // a burst: 20 Hz x 8 friends for one second, sent at once, all arrive in order
    let before = b.frames();
    for i in 0..160 {
        a.udp(b.vip, 47210, 47210, i.to_string().as_bytes(), 1, 0);
    }
    b.until_ms(3000, |g| (g.frames.len() == before + 160).then_some(())).await;
    let got: Vec<usize> = b.got().frames[before..].iter().map(|f| String::from_utf8_lossy(&f.0[16..]).parse().unwrap()).collect();
    assert_eq!(got, (0..160).collect::<Vec<_>>());
    a.close().await;
    b.close().await;
    sw.close().await;
}

// what a peer learns about the others: virtual addresses and cleaned names, nothing else
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn what_a_peer_learns_about_the_others() {
    let _s = serial().await;
    let sw = start(opts()).await;
    let mut a = Client::connect(sw.port, "Alice").await;
    let mut b = Client::with(sw.port, "  B\u{202E}ob\u{0}\u{200B}\n\tthe\u{7} driver with a very long name indeed  ", "", deltas()).await;
    a.welcome().await;
    b.welcome().await;
    // bidi, zero-width and control characters gone, spaces folded, 32 characters
    let want = "Bob the driver with a very long";
    a.until_true(|g| g.last_peers().is_some_and(|p| p.iter().any(|p| p["name"].as_str().unwrap().starts_with("Bob")))).await;
    a.text(r#"{"t":"ping","id":1}"#);
    a.until_true(|g| g.ctl.iter().any(|m| m["t"] == "pong")).await;
    b.udp(a.vip, 1, 2, b"x", 1, 0);
    a.until_true(|g| !g.frames.is_empty()).await;
    let names = a.got().names();
    assert_eq!(names, format!("Alice,{want}"));
    assert!(want.chars().count() <= 32);
    // every control message has only these fields, and no address but a virtual one
    b.until_true(|g| g.count("delta") > 0).await;
    let shape = |t: &str| -> Vec<&str> {
        match t {
            "welcome" => vec!["bundle", "mask", "t", "token", "vip", "vipStr"],
            "peers" => vec!["peers", "t"],
            "delta" => vec!["gone", "peers", "t"],
            "pong" => vec!["id", "t"],
            t => panic!("a message of kind {t}"),
        }
    };
    let all: Vec<Value> = a.got().ctl.iter().chain(b.got().ctl.iter()).cloned().collect();
    for m in &all {
        let mut keys: Vec<&str> = m.as_object().unwrap().keys().map(|k| k.as_str()).collect();
        keys.sort();
        assert_eq!(keys, shape(m["t"].as_str().unwrap()), "fields of {}", m["t"]);
        if m["t"] == "peers" || m["t"] == "delta" {
            for p in m["peers"].as_array().unwrap() {
                let mut k: Vec<&str> = p.as_object().unwrap().keys().map(|k| k.as_str()).collect();
                k.sort();
                assert_eq!(k, ["name", "vip"]);
            }
        }
        if m["t"] == "delta" {
            assert!(m["gone"].as_array().unwrap().iter().all(|v| v.is_u64()), "gone: addresses only");
        }
        let s = m.to_string();
        for ip4 in dotted_quads(&s) {
            assert!(
                ip4.strip_prefix("10.77.").and_then(|r| r.split('.').next()).and_then(|b| b.parse::<u8>().ok()).is_some_and(|b| b <= 19)
                    || ip4 == "255.255.224.0",
                "only virtual addresses ({ip4})"
            );
        }
        assert!(!s.contains("127.0.0.1") && !s.contains("::1") && !s.contains("::ffff"), "no real address");
    }
    // a peer's token reaches that peer only
    let wa = a.got().ctl.iter().find(|m| m["t"] == "welcome").cloned().unwrap();
    let wb = b.got().ctl.iter().find(|m| m["t"] == "welcome").cloned().unwrap();
    let ta = wa["token"].as_str().unwrap();
    assert!(ta.len() == 22 && ta.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'-'));
    let tb = wb["token"].as_str().unwrap();
    let seen = a.got().ctl.iter().any(|m| m.to_string().contains(tb))
        || a.got().frames.iter().any(|f| f.0.windows(tb.len()).any(|w| w == tb.as_bytes()));
    assert!(!seen, "A never sees B's token");
    // the frame's header carries the sender's virtual address only
    assert_eq!(u32::from_be_bytes(a.got().frames[0].0[4..8].try_into().unwrap()), b.vip);
    assert!(sw.info().await["peers"][0].get("from").is_none(), "the switch keeps no real address");
    a.close().await;
    b.close().await;
    sw.close().await;
}

/// the dotted quads in a text (what /(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})/g finds)
fn dotted_quads(s: &str) -> Vec<String> {
    let b = s.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        let mut j = i;
        let mut parts = 0;
        loop {
            let st = j;
            while j < b.len() && b[j].is_ascii_digit() && j - st < 3 {
                j += 1;
            }
            if j == st {
                break;
            }
            parts += 1;
            if parts == 4 || j >= b.len() || b[j] != b'.' {
                break;
            }
            j += 1;
        }
        if parts == 4 {
            out.push(s[i..j].to_string());
            i = j;
        } else {
            i += 1;
        }
    }
    out
}

// the peer list: the page hears what changed, a page of before the whole list; a name is the first hello's
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_peer_list_changes_to_the_page_the_whole_list_to_a_page_of_before() {
    let _s = serial().await;
    let sw = start(opts()).await;
    let mut a = Client::with(sw.port, "Alice", "", deltas()).await;
    let mut b = Client::connect(sw.port, "Bob").await;
    let wa = a.welcome().await;
    b.welcome().await;
    for c in [&a, &b] {
        c.until_true(|g| g.names() == "Alice,Bob").await;
    }
    // Carol joins: the page gets her as a change, not the whole list again; the page of before the whole list
    let mut c = Client::with(sw.port, "Carol", "", deltas()).await;
    c.welcome().await;
    for x in [&a, &b] {
        x.until_true(|g| g.names() == "Alice,Bob,Carol").await;
    }
    assert_eq!(a.got().count("peers"), 1, "the whole list once: at the page's hello");
    assert!(a.got().ctl.iter().any(|m| m["t"] == "delta" && m["peers"].as_array().unwrap().iter().any(|p| p["name"] == "Carol")));
    assert_eq!(b.got().count("delta"), 0, "a page of before gets no changes it cannot read");
    // more hellos from Carol (the audit: each sent everyone the whole list): her name stays, nobody hears a thing
    sleep(300).await;
    let (n0, m0) = (a.got().ctl.len(), b.got().ctl.len());
    for i in 0..30 {
        c.text(&format!(r#"{{"t":"hello","name":"Mallory{i}","deltas":1}}"#));
    }
    c.text(r#"{"t":"ping","id":5}"#);
    c.until_true(|g| g.ctl.iter().any(|m| m["t"] == "pong" && m["id"] == 5)).await;
    sleep(1300).await;
    assert_eq!(a.got().ctl.len(), n0, "nothing more to the page");
    assert_eq!(b.got().ctl.len(), m0, "nothing more to the page of before");
    assert_eq!(a.got().names(), "Alice,Bob,Carol");
    // joins in a burst: one change to each player, the whole list to the page of before at most once a second
    let (d0, f0) = (a.got().ctl.len(), b.got().ctl.len());
    let mut burst = Vec::new();
    for i in 0..5 {
        burst.push(Client::with(sw.port, &format!("P{i}"), "", Opts { deltas: i % 2 == 0, ..Opts::default() }).await);
    }
    for x in burst.iter_mut() {
        x.welcome().await;
    }
    let all = "Alice,Bob,Carol,P0,P1,P2,P3,P4";
    for x in [&a, &b] {
        x.until_true(|g| g.names() == all).await;
    }
    for x in &burst {
        x.until_true(|g| g.names() == all).await;
    }
    assert!(a.got().ctl.len() - d0 <= 2, "the page: {} messages for 5 joins", a.got().ctl.len() - d0);
    assert!(b.got().ctl.len() - f0 <= 2, "the page of before: {} whole lists for 5 joins", b.got().ctl.len() - f0);
    // a player who leaves and is back (with its token) before the change goes out: gone, then back
    sleep(1100).await;
    let cv = c.vip;
    let wc = c.got().ctl.iter().find(|m| m["t"] == "welcome").cloned().unwrap();
    c.terminate();
    let mut c2 = Client::new(sw.port, "Carol", "", Some((cv, wc["token"].as_str().unwrap())), deltas()).await;
    assert_eq!(vip_of(&c2.welcome().await), cv);
    let back =
        a.until(|g| g.ctl.iter().find(|m| m["t"] == "delta" && m["gone"].as_array().unwrap().iter().any(|v| v == cv)).cloned()).await;
    assert!(back["peers"].as_array().unwrap().iter().any(|p| p["vip"] == cv && p["name"] == "Carol"), "gone and back in one change");
    for x in [&a, &b] {
        x.until_true(|g| g.names() == all).await;
    }
    // leaves: off every list
    c2.close().await;
    for x in &burst {
        x.close().await;
    }
    for x in [&a, &b] {
        x.until_true(|g| g.names() == "Alice,Bob").await;
    }
    assert!(!a.got().names().is_empty() && !wa["vip"].is_null());
    a.close().await;
    b.close().await;
    sw.close().await;
}

// the cost of the peer list: a rename costs nothing, a join one small change to each player (the security review)
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_cost_of_the_peer_list_a_rename_costs_nothing() {
    let _s = serial().await;
    let sw = start(LanOptions { max_peers: 250, ..opts() }).await;
    // 100 players, 10 of them with a page of before
    const N: usize = 100;
    const PRE: usize = 10;
    let mut players = Vec::new();
    for i in 0..N {
        players
            .push(Client::with(sw.port, &format!("Driver {i:03}"), &format!("?g={i}"), Opts { deltas: i >= PRE, ..Opts::default() }).await);
    }
    for p in players.iter_mut() {
        p.welcome().await;
    }
    for p in &players {
        p.until_ms(5000, |g| g.view.as_ref().is_some_and(|v| v.len() == N).then_some(())).await;
    }
    sleep(1200).await;
    let bytes = |ps: &[Client]| ps.iter().map(|p| p.got().ctl_bytes).sum::<usize>();
    // a client sends 40 hellos with new names: before, each sent the whole list (4 KB) to all 100
    let b0 = bytes(&players);
    let mut m = Client::with(sw.port, "M", "?g=m", deltas()).await;
    m.welcome().await;
    m.until_true(|g| g.view.as_ref().is_some_and(|v| v.len() == N + 1)).await;
    for i in 0..40 {
        let n = if i % 2 == 1 { "A".repeat(32) } else { "B".repeat(32) };
        m.text(&format!(r#"{{"t":"hello","name":"{n}"}}"#));
    }
    sleep(1300).await;
    let rename = bytes(&players) - b0;
    // its join (one change to each page, the whole list to the 10 of before) and nothing for the renames
    let list = serde_json::json!({ "t": "peers", "peers": players[0].got().last_peers().unwrap() }).to_string().len();
    println!(
        "peer list at {} players: a join and 40 renames sent the others {rename} bytes (the whole list {list} bytes; before, 41 whole lists to each: {} bytes)",
        N + 1,
        41 * list * N
    );
    assert!(rename < (N - PRE) * 120 + PRE * (list + 200), "{rename} bytes");
    // churn: one client opens and closes connections 10 times; each costs the pages one small change, the pages of
    // before one whole list a second
    let b0 = bytes(&players);
    let t0 = Instant::now();
    for _ in 0..10 {
        let mut x = Client::with(sw.port, "churn", "?g=m", deltas()).await;
        x.welcome().await;
        x.close().await;
    }
    sleep(1300).await;
    let churn = bytes(&players) - b0;
    let secs = t0.elapsed().as_secs_f64();
    println!("10 joins and leaves in {secs:.1} s: {churn} bytes to the others");
    assert!(churn < 20 * (N - PRE) * 120 + (secs + 1.0).ceil() as usize * PRE * (list + 200), "{churn} bytes");
    m.close().await;
    for p in &players {
        p.terminate();
    }
    sw.close().await;
}

// limits: players, frame size, frame rate, control messages
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn limits_players_frame_size_frame_rate_control_messages() {
    let _s = serial().await;
    let sw = start(LanOptions { max_peers: 2, max_frame: 1600, frame_rate: 50.0, text_rate: 5.0, max_text: 256, ..opts() }).await;
    assert!(!sw.lan.full());
    let mut a = Client::connect(sw.port, "A").await;
    a.welcome().await;
    let mut b = Client::connect(sw.port, "B").await;
    b.welcome().await;
    assert!(sw.lan.full(), "full: maxPeers on the switch");
    // a third player: the switch is full (1013, a fixed reason)
    let c = Client::connect(sw.port, "C").await;
    assert_eq!(c.closed().await, (1013, "LAN full".to_string()));
    // a frame over maxFrame is dropped, one at the limit goes through
    a.udp(b.vip, 1, 2, &vec![0; 1600 - 16 + 1], 1, 0);
    a.udp(b.vip, 1, 2, &vec![0; 1600 - 16], 1, 0);
    b.until_true(|g| g.frames.len() == 1).await;
    assert_eq!(b.got().frames[0].0.len(), 1600);
    // frame rate: a burst of 500 at 50/s (bucket of 2 s = 100) loses the rest
    for _ in 0..500 {
        a.udp(b.vip, 1, 2, b"x", 1, 0);
    }
    sleep(300).await;
    let got = b.frames() - 1;
    assert!((95..=120).contains(&got), "burst limited to about two seconds' worth ({got})");
    // control messages: over maxText dropped; a flood of pings answered up to the bucket (2 s x 5)
    a.text(&format!(r#"{{"t":"ping","id":99,"pad":"{}"}}"#, "x".repeat(300)));
    for i in 0..50 {
        a.text(&format!(r#"{{"t":"ping","id":{i}}}"#));
    }
    sleep(300).await;
    let pongs: Vec<Value> = a.got().ctl.iter().filter(|m| m["t"] == "pong").cloned().collect();
    assert!(!pongs.iter().any(|m| m["id"] == 99), "an oversized control message is ignored");
    assert!((9..=12).contains(&pongs.len()), "pings answered up to the bucket ({})", pongs.len());
    assert!(sw.info().await["limited"].as_u64().unwrap() >= 400);
    a.close().await;
    b.close().await;
    sw.close().await;
}

// keepalive: pings well under a proxy idle cut, dead and idle connections closed
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn keepalive_pings_dead_and_idle_connections_closed() {
    let _s = serial().await;
    let sw = start(LanOptions { keepalive_ms: 150, idle_ms: 600, ..opts() }).await;
    // a live client answers pings (tungstenite does by itself)
    let mut a = Client::connect(sw.port, "A").await;
    a.welcome().await;
    // a dead one does not answer: gone after two intervals
    let dead = drain(raw_ws(sw.port, "").await);
    let t0 = Instant::now();
    dead.await.unwrap();
    let t_dead = ms_since(t0);
    assert!(t_dead < 600.0, "no pong: closed after {t_dead:.0} ms");
    // (the tick that cut the dead one pinged A too: that ping may be on its way still)
    let _ = tokio::time::timeout(std::time::Duration::from_millis(100), a.until_true(|g| g.pings >= 2)).await;
    let pings = a.got().pings;
    assert!(pings >= 2, "pings to a live client ({pings})");
    // A answers pings but never sends anything: closed once idle for idleMs
    let t1 = Instant::now();
    a.closed().await;
    let t_idle = ms_since(t1);
    assert!(t_idle < 1200.0, "idle client closed ({t_idle:.0} ms)");
    sw.close().await;
}

// control frames: pings answered a few a second, a ping flood cut, unasked pongs never count
// (WebSocket control frames: the switch answers pings itself, a few a second; a client that floods them, or sends
// pongs nobody asked for, is cut, and only a pong to the switch's own ping counts as a sign of life: the phase-2
// deploy review's finding 1)
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn control_frames_pings_answered_a_flood_cut_unasked_pongs_never_count() {
    let _s = serial().await;
    let sw = start(LanOptions { keepalive_ms: 200, ctl_rate: 10.0, ..opts() }).await;
    // a few pings: each answered exactly once, with its own payload
    let a = Client::connect(sw.port, "A").await;
    for i in 0..5 {
        a.ping(format!("p{i}").into_bytes());
    }
    sleep(200).await;
    let pongs: Vec<String> = a.got().pongs.iter().map(|p| String::from_utf8_lossy(p).to_string()).collect();
    assert_eq!(pongs, ["p0", "p1", "p2", "p3", "p4"], "one pong per ping, payload echoed");
    // a flood from a client that never reads: cut at once, whatever it sends next
    let mut f = raw_ws(sw.port, "").await;
    let one = frame(PING, &[0x41; 125]);
    let burst: Vec<u8> = one.iter().copied().cycle().take(one.len() * 512).collect();
    let mut sent = 0usize;
    let t0 = Instant::now();
    let mut destroyed = false;
    while t0.elapsed().as_millis() < 3000 {
        match tokio::time::timeout(std::time::Duration::from_millis(500), f.write_all(&burst)).await {
            Ok(Ok(())) => {}
            _ => {
                destroyed = true;
                break;
            }
        }
        sent += burst.len();
        if sent.is_multiple_of(burst.len() * 8) && f.write_all(&frame(PONG, &[])).await.is_err() {
            destroyed = true;
            break;
        }
    }
    if !destroyed {
        // the switch's end of it: a read finds it closed
        let mut b = vec![0u8; 1 << 16];
        let t = Instant::now();
        while t.elapsed().as_millis() < 2000 {
            match tokio::time::timeout(std::time::Duration::from_millis(100), f.read(&mut b)).await {
                Ok(Ok(0)) | Ok(Err(_)) => {
                    destroyed = true;
                    break;
                }
                _ => {}
            }
        }
    }
    assert!(destroyed, "the flooding client is cut (sent {:.1} MB)", sent as f64 / 1048576.0);
    assert!(sent < 64 << 20, "cut early (after {:.1} MB)", sent as f64 / 1048576.0);
    // a client that never answers the switch's pings but sends pongs of its own, a few a second: still dead
    let d = raw_ws(sw.port, "").await;
    let (mut dr, mut dw) = d.into_split();
    let unasked = tokio::spawn(async move {
        loop {
            if dw.write_all(&frame(PONG, b"alive?")).await.is_err() {
                return;
            }
            sleep(150).await;
        }
    });
    let t1 = Instant::now();
    let closed = tokio::time::timeout(std::time::Duration::from_millis(3000), async {
        let mut b = vec![0u8; 4096];
        loop {
            match dr.read(&mut b).await {
                Ok(0) | Err(_) => return,
                Ok(_) => {}
            }
        }
    })
    .await;
    unasked.abort();
    assert!(closed.is_ok(), "unasked pongs do not keep a connection alive");
    assert!(t1.elapsed().as_millis() < 1500, "closed by the keepalive ({} ms)", t1.elapsed().as_millis());
    // a client that answers the switch's pings stays (tungstenite does by itself)
    let p0 = a.got().pings;
    a.text(r#"{"t":"hello","name":"A"}"#);
    sleep(700).await;
    assert!(a.is_open(), "a live client stays");
    let pinged = a.got().pings - p0;
    assert!(pinged >= 2, "pinged ({pinged})");
    a.close().await;
    sw.close().await;
}

// a receiver that never reads: cut by the keepalive tick once its queue is over the limit
// B sends to A, which never reads, until A's queue on the switch is just over the limit, then stops: no further
// send finds it, the next keepalive tick does (before the missing pong would).  The kernel's buffers come first:
// here they are kept small (the switch's SO_SNDBUF, the server's NFSU2_LAN_SNDBUF, and A's SO_RCVBUF), as Linux
// otherwise grows them to megabytes and the missing pong ends A first (the test fails that way with Linux's sizes);
// and "over the limit" counts once it stays so for 100 ms (before the kernel's buffers are full, a delayed ACK
// frees them again a moment later).
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_receiver_that_never_reads_is_cut_by_the_keepalive_tick() {
    let _s = serial().await;
    let sw = start(LanOptions {
        keepalive_ms: 400,
        send_buffer: 16 << 10,
        shed_queue: 1 << 20,
        byte_rate: (64 << 20) as f64,
        frame_rate: 100000.0,
        group_byte_rate: (64 << 20) as f64,
        group_frame_rate: 100000.0,
        peers_ms: 1,
        full_peers_ms: 1.0,
        send_buffer_os: 32 << 10,
        ..opts()
    })
    .await;
    let a = raw_ws_with(sw.port, "", Some(8 << 10)).await;
    let mut b = Client::connect(sw.port, "B").await;
    b.welcome().await;
    sleep(50).await;
    let pa = sw.peers().into_iter().find(|p| p.name != "B").unwrap();
    let buffered = |sw: &Switch| sw.peers().iter().find(|p| p.vip == pa.vip).map_or(0, |p| p.buffered);
    let mut n = 0;
    let t_flood = Instant::now();
    while n < 20000 {
        if buffered(&sw) > 16 << 10 {
            // over the limit: for good once the kernel's buffers are full (an ACK still frees them a moment later
            // while they are not)
            sleep(100).await;
            if buffered(&sw) > 16 << 10 {
                break;
            }
            continue;
        }
        let mut f = vec![0u8; 8000];
        f[0] = 1;
        f[8..12].copy_from_slice(&pa.vip.to_be_bytes());
        b.binary(f);
        n += 1;
        // let the switch take it
        b.binary(Vec::new());
        b.flush().await;
        sleep(1).await;
    }
    let q = buffered(&sw);
    println!("A's queue {q} bytes after {n} frames in {:.0} ms", ms_since(t_flood));
    assert!(q > 16 << 10, "A's queue grew ({q} bytes after {n} frames)");
    let t0 = Instant::now();
    let mut seen = Vec::new();
    while sw.has(pa.vip) && t0.elapsed().as_millis() < 1500 {
        seen.push(buffered(&sw));
        sleep(20).await;
    }
    println!("A's queue every 20 ms until it went: {seen:?}");
    assert!(!sw.has(pa.vip), "A is off the switch");
    let cuts = sw.info().await["cuts"].clone();
    assert_eq!(cuts["queue"], 1, "cut by the tick for its queue: {cuts}");
    b.close().await;
    drop(a);
    sw.close().await;
}

// LAN impairment (--lan-impair): UDP lost, TCP delayed in order, free roam untouched
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn lan_impairment_udp_lost_tcp_delayed_in_order_free_roam_untouched() {
    let _s = serial().await;
    let sw = start_full(opts(), "delay=80,jitter=0,loss=100,seed=3", None, None, None).await;
    let mut a = Client::connect(sw.port, "A").await;
    a.welcome().await;
    let mut b = Client::connect(sw.port, "B").await;
    b.welcome().await;
    // LAN UDP (kind 3) and ICMP (kind 5): every one lost at loss=100
    a.udp(b.vip, 9999, 9999, b"udp", 3, 0);
    a.udp(b.vip, 0, 0, b"icmp", 5, 0);
    // free roam (kind 1): not impaired, arrives at once
    let t0 = Instant::now();
    a.udp(b.vip, 47210, 47210, b"roam", 1, 0);
    let r = b.until(|g| g.frames.iter().find(|f| f.0[0] == 1).map(|f| f.1)).await;
    let ms = r.duration_since(t0).as_secs_f64() * 1000.0;
    assert!(ms < 60.0, "kind 1 not delayed ({ms:.1} ms)");
    // LAN TCP (kind 4): never lost, delayed by 80 ms, in order
    let t1 = Instant::now();
    for i in 0..5 {
        a.udp(b.vip, 50000, 9900, format!("seg{i}").as_bytes(), 4, 0);
    }
    b.until_ms(3000, |g| (g.frames.iter().filter(|f| f.0[0] == 4).count() == 5).then_some(())).await;
    let tcp: Vec<(String, Instant)> =
        b.got().frames.iter().filter(|f| f.0[0] == 4).map(|f| (String::from_utf8_lossy(&f.0[16..]).to_string(), f.1)).collect();
    assert_eq!(tcp.iter().map(|t| t.0.as_str()).collect::<Vec<_>>(), ["seg0", "seg1", "seg2", "seg3", "seg4"], "in order");
    let d = tcp[0].1.duration_since(t1).as_secs_f64() * 1000.0;
    assert!(d >= 75.0, "TCP delayed ({d:.1} ms)");
    sleep(200).await;
    assert_eq!(b.got().frames.iter().filter(|f| f.0[0] == 3 || f.0[0] == 5).count(), 0, "UDP and ICMP lost");
    assert_eq!(sw.info().await["lost"], 2);
    a.close().await;
    b.close().await;
    sw.close().await;
}

// LAN impairment's hold (a lost TCP segment on a player's path): a link's datagrams wait for the hold's end, then come
// at once, in the order they were sent; another link has holds of its own
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn lan_impairment_hold_releases_a_links_datagrams_at_once_in_order() {
    let _s = serial().await;
    let sw = start_full(opts(), "hold=100,holdms=300,seed=5", None, None, None).await;
    let mut a = Client::connect(sw.port, "A").await;
    a.welcome().await;
    let mut b = Client::connect(sw.port, "B").await;
    b.welcome().await;
    let t0 = Instant::now();
    for i in 0..5 {
        a.udp(b.vip, 3658, 3658, format!("d{i}").as_bytes(), 3, 0);
        sleep(40).await;
    }
    b.until_ms(3000, |g| (g.frames.iter().filter(|f| f.0[0] == 3).count() == 5).then_some(())).await;
    let got: Vec<(String, f64)> = b
        .got()
        .frames
        .iter()
        .filter(|f| f.0[0] == 3)
        .map(|f| (String::from_utf8_lossy(&f.0[16..]).to_string(), f.1.duration_since(t0).as_secs_f64() * 1000.0))
        .collect();
    println!("held: {got:?}");
    assert_eq!(got.iter().map(|g| g.0.as_str()).collect::<Vec<_>>(), ["d0", "d1", "d2", "d3", "d4"], "in order");
    assert!(got[0].1 >= 290.0, "held until the hold's end: {got:?}");
    assert!(got[4].1 - got[0].1 < 30.0, "then at once: {got:?}");
    // the other way, B to A, is a link of its own: its first datagram starts its own hold
    let t1 = Instant::now();
    b.udp(a.vip, 3658, 3658, b"back", 3, 0);
    let r = a.until(|g| g.frames.iter().find(|f| f.0[0] == 3).map(|f| f.1)).await;
    let ms = r.duration_since(t1).as_secs_f64() * 1000.0;
    assert!(ms >= 290.0, "B to A held too ({ms:.1} ms)");
    let im = sw.info().await["impair"].clone();
    assert_eq!((im["hold"].clone(), im["holdms"].clone()), (100.into(), 300.into()), "{im}");
    a.close().await;
    b.close().await;
    sw.close().await;
}

// LAN impairment's legs (each player's own way to the server): a datagram takes its sender's leg up and its
// receiver's leg down; one passed on by a third browser takes four legs; leg.N is browser N's own
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn lan_impairment_legs_add_up_per_player() {
    let _s = serial().await;
    let sw = start_full(opts(), "leg=60", None, None, None).await;
    let mut a = Client::connect(sw.port, "A").await;
    a.welcome().await;
    let mut b = Client::connect(sw.port, "B").await;
    b.welcome().await;
    let mut c = Client::connect(sw.port, "C").await;
    c.welcome().await;
    // A to B straight: 120 ms; A to C, then C passing it on to B: 240 ms
    let t0 = Instant::now();
    a.udp(c.vip, 3658, 3658, b"via-c", 3, 0);
    a.udp(b.vip, 3659, 3659, b"straight", 3, 0);
    c.until(|g| g.frames.iter().find(|f| f.0[0] == 3).map(|f| f.1)).await;
    c.udp(b.vip, 3658, 3658, b"via-c", 3, 0);
    b.until_ms(3000, |g| (g.frames.iter().filter(|f| f.0[0] == 3).count() == 2).then_some(())).await;
    let got: Vec<(String, f64)> = b
        .got()
        .frames
        .iter()
        .filter(|f| f.0[0] == 3)
        .map(|f| (String::from_utf8_lossy(&f.0[16..]).to_string(), f.1.duration_since(t0).as_secs_f64() * 1000.0))
        .collect();
    println!("legs: {got:?}");
    assert_eq!(got[0].0, "straight", "{got:?}");
    assert!((115.0..200.0).contains(&got[0].1) && (235.0..340.0).contains(&got[1].1), "{got:?}");
    assert_eq!(sw.info().await["impair"]["leg"], 60);
    a.close().await;
    b.close().await;
    c.close().await;
    sw.close().await;
    // leg.2: the first browser on the switch (host number 2) has a way of its own
    let sw = start_full(opts(), "leg=60,leg.2=150", None, None, None).await;
    let mut a = Client::connect(sw.port, "A").await;
    a.welcome().await;
    let mut b = Client::connect(sw.port, "B").await;
    b.welcome().await;
    assert_eq!(a.vip & 0x1fff, 2);
    let t0 = Instant::now();
    a.udp(b.vip, 3658, 3658, b"own", 3, 0);
    let r = b.until(|g| g.frames.iter().find(|f| f.0[0] == 3).map(|f| f.1)).await;
    let ms = r.duration_since(t0).as_secs_f64() * 1000.0;
    assert!((205.0..300.0).contains(&ms), "A's own leg (150) and B's (60): {ms:.1} ms");
    a.close().await;
    b.close().await;
    sw.close().await;
}

// ---- the audit's F1: a departed player's address and the frames meant for it ----

// a departed player's address goes only to its token: an attacker who asks first gets nothing of it
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_departed_players_address_goes_only_to_its_token() {
    let _s = serial().await;
    let sw = start(LanOptions { reserve_ms: 1500, ..opts() }).await;
    let mut alice = Client::connect(sw.port, "Alice").await;
    alice.welcome().await;
    let mut bob = Client::connect(sw.port, "Bob").await;
    let wb = bob.welcome().await;
    let mut mallory = Client::connect(sw.port, "Mallory").await;
    mallory.welcome().await;
    mallory.until_true(|g| g.last_peers().is_some_and(|p| p.len() == 3)).await;
    // Bob drops; Mallory sees it in the peer list and asks for Bob's address at once, every way it can
    let bvip = bob.vip;
    bob.terminate();
    mallory.until_true(|g| g.last_peers().is_some_and(|p| p.len() == 2 && !p.iter().any(|p| p["vip"] == bvip))).await;
    let b22 = "B".repeat(22);
    let mut grabs = Vec::new();
    for i in 0..4 {
        let resume = (i % 2 == 1).then_some((bvip, b22.as_str()));
        grabs.push(Client::new(sw.port, "Bob", &format!("?vip={}", ip_str(bvip)), resume, Opts::default()).await);
    }
    let mut got = Vec::new();
    for g in grabs.iter_mut() {
        got.push(g.welcome().await);
    }
    assert!(
        got.iter().all(|w| vip_of(w) != bvip),
        "Mallory's connections got {:?}, never {}",
        got.iter().map(|w| w["vipStr"].clone()).collect::<Vec<_>>(),
        ip_str(bvip)
    );
    // Bob's page comes back 250 ms later with its token: its own address
    sleep(250).await;
    let mut bob2 = Client::new(sw.port, "Bob", "", Some((bvip, wb["token"].as_str().unwrap())), Opts::default()).await;
    assert_eq!(vip_of(&bob2.welcome().await), bvip, "Bob gets its address back");
    // Alice's frame for Bob reaches Bob, and none of Mallory's connections
    alice.udp(bvip, 9999, 9999, b"race-data-for-Bob", 3, 0);
    let f = bob2.until(|g| g.frames.first().map(|f| f.0.clone())).await;
    assert_eq!(&f[16..], b"race-data-for-Bob");
    sleep(100).await;
    assert_eq!(mallory.frames() + grabs.iter().map(|g| g.frames()).sum::<usize>(), 0, "nothing for Bob reached the attacker");
    // one Bob on the switch at Bob's address (the peer list comes up to a second later)
    alice.until_true(|g| g.last_peers().is_some_and(|p| p.iter().filter(|p| p["vip"] == bvip).count() == 1)).await;
    // a reservation ends: after reserveMs the address is an ordinary free one again (no token needed)
    bob2.close().await;
    sleep(50).await;
    assert!(sw.info().await["reserved"].as_u64().unwrap() >= 1, "reserved while it waits for its token");
    sleep(1700).await;
    sw.lan.keepalive();
    assert!(!sw.reserved().contains(&bvip), "the reservation ended");
    for c in [&alice, &mallory] {
        c.close().await;
    }
    for c in &grabs {
        c.close().await;
    }
    sw.close().await;
}

// a resume replaces the same client's older connection (one that died without a close), and only with the token
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_resume_replaces_the_same_clients_older_connection_only_with_the_token() {
    let _s = serial().await;
    let sw = start(opts()).await;
    let mut a = Client::connect(sw.port, "Alice").await;
    a.welcome().await;
    let mut b = Client::connect(sw.port, "Bob").await;
    let wb = b.welcome().await;
    // a wrong token while Bob is still connected: a fresh address, Bob stays
    let c22 = "C".repeat(22);
    let mut x = Client::new(sw.port, "Bob", "", Some((b.vip, &c22)), Opts::default()).await;
    let wx = x.welcome().await;
    assert_ne!(vip_of(&wx), b.vip);
    assert!(b.is_open());
    // Bob's page thinks its connection is gone and comes back with its token: the old one is closed
    let mut b2 = Client::new(sw.port, "Bob", "", Some((b.vip, wb["token"].as_str().unwrap())), Opts::default()).await;
    let wb2 = b2.welcome().await;
    assert_eq!(vip_of(&wb2), b.vip);
    b.closed().await;
    sleep(50).await;
    assert!(sw.peers().iter().any(|p| p.vip == b.vip && p.open), "the new connection holds the address after the old one closed");
    a.udp(b.vip, 1, 2, b"to the new one", 1, 0);
    assert_eq!(&b2.until(|g| g.frames.first().map(|f| f.0.clone())).await[16..], b"to the new one");
    // Bob, and the wrong-token client
    a.until_true(|g| g.last_peers().is_some_and(|p| p.iter().filter(|p| p["name"] == "Bob").count() == 2)).await;
    x.close().await;
    a.close().await;
    b2.close().await;
    sw.close().await;
}

// reservations: never handed to another client, not even when every address is held or reserved (LAN full instead); a
// silent ?resume times out to a fresh one
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn reservations_never_handed_to_another_client_a_silent_resume_times_out() {
    let _s = serial().await;
    let sw = start(LanOptions { max_peers: 6000, resume_wait_ms: 300, ..opts() }).await;
    let now = nfsu2_util::now_ms();
    for (i, v) in all_vips().into_iter().enumerate() {
        let mut h = [0u8; 32];
        nfsu2_util::rand::fill(&mut h);
        sw.lan.reserve_for_test(v, h, now + 60_000 + i as i64, Some("others"), None);
    }
    let c = Client::with(sw.port, "C", "?g=newcomer", Opts::default()).await;
    assert_eq!(c.closed().await, (1013, "LAN full".into()), "a newcomer waits for an address to free; nobody's reservation is given away");
    assert_eq!(sw.info().await["reserved"], 6000, "every reservation kept");
    sw.lan.clear_reserved();
    let t0 = Instant::now();
    let (ws, _) = tokio_tungstenite::connect_async(format!("ws://127.0.0.1:{}/lan?resume", sw.port)).await.unwrap();
    let (_, mut rd) = futures_util::StreamExt::split(ws);
    let m = loop {
        match futures_util::StreamExt::next(&mut rd).await.unwrap().unwrap() {
            tokio_tungstenite::tungstenite::Message::Text(t) => break serde_json::from_str::<Value>(t.as_str()).unwrap(),
            _ => continue,
        }
    };
    assert_eq!(m["t"], "welcome");
    assert!(t0.elapsed().as_millis() >= 250, "after the wait");
    sw.close().await;
}

// the review of F1: with every address held or reserved, opening and closing connections fast enough made the
// switch hand out the oldest reservation, a dropped player's address, within 60 ms
// churning connections never takes a dropped player's address; each client keeps only its own share of reservations
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn churning_connections_never_takes_a_dropped_players_address() {
    let _s = serial().await;
    let sw = start(LanOptions { max_peers: 6000, group_reserve: 8, site_reserve: 16, ..opts() }).await;
    let mut alice = Client::with(sw.port, "Alice", "?g=alice", Opts::default()).await;
    alice.welcome().await;
    let mut bob = Client::with(sw.port, "Bob", "?g=bob", Opts::default()).await;
    let wb = bob.welcome().await;
    // the rest of the addresses held by others, so that the switch has no free one left once Bob drops
    let now = nfsu2_util::now_ms();
    for v in all_vips() {
        if v != alice.vip && v != bob.vip {
            let mut h = [0u8; 32];
            nfsu2_util::rand::fill(&mut h);
            sw.lan.reserve_for_test(v, h, now + 60_000, Some("others"), None);
        }
    }
    let bvip = bob.vip;
    bob.terminate();
    alice.until_true(|g| !g.last_peers().unwrap_or_default().iter().any(|p| p["vip"] == bvip)).await;
    assert!(sw.lan.full(), "full with one player on: every other address kept for its player");
    // Mallory opens and closes connections as fast as it can, from many addresses of its own (4000 at most: a refusal
    // takes microseconds, and a Mac's 16,384 ports for connections ran out within the 600 ms, each kept 30 s)
    let (mut got, mut full) = (0, 0);
    let t0 = Instant::now();
    while t0.elapsed().as_millis() < 600 && full < 4000 {
        let ends: Vec<_> = (0..20)
            .map(|i| {
                let port = sw.port;
                tokio::spawn(async move {
                    let Ok((ws, _)) = tokio_tungstenite::connect_async(format!("ws://127.0.0.1:{port}/lan?g=m{}&s=mallory", i % 5)).await
                    else {
                        return 0u32;
                    };
                    let (_, mut rd) = futures_util::StreamExt::split(ws);
                    while let Some(Ok(m)) = futures_util::StreamExt::next(&mut rd).await {
                        match m {
                            tokio_tungstenite::tungstenite::Message::Text(t) => {
                                let w: Value = serde_json::from_str(t.as_str()).unwrap();
                                if w["t"] == "welcome" {
                                    return vip_of(&w);
                                }
                            }
                            tokio_tungstenite::tungstenite::Message::Close(f) => return f.map_or(1005, |f| u16::from(f.code) as u32),
                            _ => {}
                        }
                    }
                    0
                })
            })
            .collect();
        for e in ends {
            let r = e.await.unwrap();
            if r == bvip {
                got += 1;
            } else if r == 1013 {
                full += 1;
            }
        }
    }
    println!(
        "churn: Mallory's connections got Bob's address {got} times; \"LAN full\" {full} times; reserved {}",
        sw.info().await["reserved"]
    );
    assert_eq!(got, 0, "never Bob's address");
    assert!(full > 0, "LAN full instead");
    let mut bob2 = Client::new(sw.port, "Bob", "?g=bob", Some((bvip, wb["token"].as_str().unwrap())), Opts::default()).await;
    assert_eq!(vip_of(&bob2.welcome().await), bvip, "Bob gets his address back");
    bob2.close().await;
    alice.close().await;
    sw.lan.expire_reserved();
    assert!(!sw.lan.full(), "not full once the kept addresses run out");
    sw.close().await;
    // a client that drops many connections keeps groupReserve reservations (its own oldest go), a site siteReserve
    let sw2 = start(LanOptions { max_peers: 6000, group_reserve: 3, site_reserve: 5, ..opts() }).await;
    async fn leave(port: u16, q: &str, n: usize) -> Vec<u32> {
        let mut cs = Vec::new();
        for _ in 0..n {
            cs.push(Client::with(port, "x", q, Opts::default()).await);
        }
        let mut v = Vec::new();
        for c in cs.iter_mut() {
            v.push(vip_of(&c.welcome().await));
        }
        for c in &cs {
            c.close().await;
        }
        sleep(20).await;
        v
    }
    let first = leave(sw2.port, "?g=one", 6).await;
    let mut kept = sw2.reserved();
    kept.sort();
    let mut newest = first[3..].to_vec();
    newest.sort();
    assert_eq!(kept, newest, "the newest 3 of one client kept, its own oldest gone");
    leave(sw2.port, "?g=two&s=site", 3).await;
    leave(sw2.port, "?g=three&s=site", 3).await;
    let r = sw2.lan.reserved_view();
    assert_eq!(r.iter().filter(|r| r.site.as_deref() == Some("site")).count(), 5, "one site keeps 5");
    assert_eq!(r.iter().filter(|r| r.group.as_deref() == Some("one")).count(), 3, "another client's reservations untouched");
    let i = sw2.info().await;
    println!("reservations: {} kept, {} given up by their own clients", i["reserved"], i["evicted"]);
    sw2.close().await;
}
