//! The switch's own tests, the second half (tests/lan.rs has the first): the audit's F2 (what one client can make the
//! switch send, and whom it can get cut), the store, the 6000 addresses, LAN play's broadcasts, a page that lets go,
//! the breakdown, bundles, a player behind on its path (lag.rs).

mod harness;

use std::collections::{HashSet, VecDeque};
use std::sync::atomic::{AtomicU64, Ordering::Relaxed};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use harness::*;
use nfsu2_lan::{Breakdown, LanOptions, LanSwitch};
use serde_json::Value;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::task::JoinHandle;

fn vip_of(m: &Value) -> u32 {
    m["vip"].as_u64().unwrap() as u32
}

fn src(f: &[u8]) -> u32 {
    u32::from_be_bytes(f[4..8].try_into().unwrap())
}

/// the options for the F2 tests: the budgets far away, so that the queue rules are what is tested
fn unlimited() -> LanOptions {
    LanOptions {
        byte_rate: (64 << 20) as f64,
        frame_rate: 100000.0,
        group_byte_rate: (64 << 20) as f64,
        group_frame_rate: 100000.0,
        ..opts()
    }
}

// a flood of datagrams never gets a slower receiver cut: they are dropped for it, as a full link drops UDP
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_flood_of_datagrams_never_gets_a_slower_receiver_cut() {
    let _s = serial().await;
    let sw = start(LanOptions { send_buffer: 256 << 10, shed_queue: 32 << 10, roam_queue: 16 << 10, ..unlimited() }).await;
    // reads nothing: the slowest possible link
    let victim = raw_peer(sw.port, "Victim", "").await;
    let mut attacker = Client::connect(sw.port, "Attacker").await;
    attacker.welcome().await;
    sleep(100).await;
    let pv = sw.peer_named("Victim").unwrap();
    // 8 KB datagrams of each kind but the stream, unicast and broadcast, far past every queue limit
    for i in 0..400 {
        let dst = if i % 4 == 3 { BROADCAST } else { pv.vip };
        attacker.udp(dst, 1, 2, &[0; 8000], [1, 2, 3, 5][i % 4], 0);
        if i % 50 == 49 {
            attacker.flush().await;
            sleep(5).await;
        }
    }
    sleep(500).await;
    let info = sw.info().await;
    let q = sw.peers().iter().find(|p| p.vip == pv.vip).map_or(0, |p| p.buffered);
    println!("datagram flood at a reader that never reads: victim queue {q} bytes, shed {}, cuts {}", info["shed"], info["cuts"]);
    assert!(sw.has(pv.vip), "the victim is still on the switch");
    assert_eq!(info["cuts"]["slow"], 0, "nobody cut for a slow queue");
    assert!(info["shed"].as_u64().unwrap() > 0, "datagrams dropped for the full queue");
    assert!(q <= (32 << 10) + 8192, "the victim's queue stays at the drop mark ({q})");
    drop(victim);
    attacker.close().await;
    sw.close().await;
}

// free roam's datagrams (kind 1) are dropped at a much shorter queue than the others: no seconds of old positions
// (The kernel's buffers to the victim are kept small, so that they are full before the switch's queue builds: a
// burst reaches the switch faster than the victim's writer hands it to the kernel, and the queue rules shed most of
// it there, where Node wrote each frame to the kernel at once; the kernel then takes the rest and the queue this
// test measures is empty.)
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn free_roams_datagrams_are_dropped_at_a_much_shorter_queue() {
    let _s = serial().await;
    let sw =
        start(LanOptions { send_buffer: 4 << 20, shed_queue: 256 << 10, roam_queue: 16 << 10, send_buffer_os: 32 << 10, ..unlimited() })
            .await;
    let victim = raw_peer_with(sw.port, "Victim", "", Some(8 << 10)).await;
    let mut sender = Client::connect(sw.port, "Sender").await;
    sender.welcome().await;
    sleep(100).await;
    let pv = sw.peer_named("Victim").unwrap();
    let queue = |sw: &Switch| sw.peers().iter().find(|p| p.vip == pv.vip).map_or(0, |p| p.buffered);
    // free roam: past the kernel's buffers, then the switch's queue stops at roamQueue
    for i in 0..400 {
        sender.udp(pv.vip, 1, 2, &[0; 8000], 1, 0);
        if i % 50 == 49 {
            sender.flush().await;
            sleep(5).await;
        }
    }
    sleep(300).await;
    let roam_q = queue(&sw);
    let roam_shed = sw.info().await["shed"].as_u64().unwrap();
    // LAN play's datagrams (kind 3) still queue up to shedQueue
    for i in 0..60 {
        sender.udp(pv.vip, 1, 2, &[0; 8000], 3, 0);
        if i % 20 == 19 {
            sender.flush().await;
            sleep(5).await;
        }
    }
    sleep(300).await;
    let lan_q = queue(&sw);
    println!("kind 1 queue {roam_q} bytes (shed {roam_shed}), then kind 3 queue {lan_q} bytes");
    assert!(roam_shed > 0, "free-roam datagrams dropped");
    assert!(roam_q > 0 && roam_q <= (16 << 10) + 8192, "free roam stops at roamQueue ({roam_q})");
    assert!(lan_q > (16 << 10) + 8192 && lan_q <= (256 << 10) + 8192, "LAN play's datagrams go on to shedQueue ({lan_q})");
    assert!(sw.has(pv.vip), "the receiver is still on the switch");
    drop(victim);
    sender.close().await;
    sw.close().await;
}

fn stream_opts() -> LanOptions {
    LanOptions {
        send_buffer: 4 << 20,
        shed_queue: 32 << 10,
        stream_queue: 512 << 10,
        stream_share: 256 << 10,
        stall_ms: 400.0,
        ..unlimited()
    }
}

// a burst of stream segments (kind 4) at a receiver: the sender is cut, not the receiver
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_burst_of_stream_segments_the_sender_is_cut_not_the_receiver() {
    let _s = serial().await;
    let sw = start(stream_opts()).await;
    let victim = raw_peer(sw.port, "Victim", "?g=victim").await;
    let mut attacker = Client::with(sw.port, "Attacker", "?g=attacker", Opts::default()).await;
    attacker.welcome().await;
    sleep(100).await;
    let pv = sw.peer_named("Victim").unwrap();
    for i in 0..300 {
        if !attacker.is_open() {
            break;
        }
        attacker.udp(pv.vip, 50000, 9900, &[0; 8000], 4, 0);
        if i % 20 == 19 {
            attacker.flush().await;
            sleep(5).await;
        }
    }
    let _ = tokio::time::timeout(std::time::Duration::from_millis(2000), attacker.closed()).await;
    let info = sw.info().await;
    println!(
        "stream burst: attacker {}, victim {}, cuts {}",
        if attacker.is_open() { "still on" } else { "cut" },
        if sw.has(pv.vip) { "still on" } else { "cut" },
        info["cuts"]
    );
    assert_eq!(info["cuts"]["hog"], 1, "the sender is cut for flooding");
    assert!(sw.has(pv.vip), "the receiver stays");
    assert_eq!(info["cuts"]["slow"], 0);
    // normal stream traffic (the LAN lobby: a few small segments) still flows
    let mut a = Client::connect(sw.port, "A").await;
    a.welcome().await;
    let mut b = Client::connect(sw.port, "B").await;
    b.welcome().await;
    for i in 0..50 {
        a.udp(b.vip, 50000, 9900, format!("seg{i}").as_bytes(), 4, 0);
    }
    b.until_true(|g| g.frames.len() == 50).await;
    drop(victim);
    a.close().await;
    b.close().await;
    sw.close().await;
}

// the review of F2: stream segments from 9 client addresses, each under its own share, got a receiver that reads
// (slower than all of them together) cut.  (The kernel's buffers are kept small here, as in the never-reads test:
// the switch's send buffer and the victim's receive buffer, which Linux grows to megabytes for a socket that reads,
// take the 2 MB more than the victim reads in the 5 s and the switch's own queue never fills; the test fails that
// way with Linux's sizes.)
// stream segments from many clients at a receiver that reads: the clients flooding it go, it stays
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn stream_segments_from_many_clients_at_a_receiver_that_reads() {
    let _s = serial().await;
    let sw = start(LanOptions { stall_ms: 1000.0, send_buffer_os: 64 << 10, ..stream_opts() }).await;
    let victim = raw_peer_with(sw.port, "Victim", "?g=victim", Some(64 << 10)).await;
    // reads about 250 KB/s
    let reading = throttle(victim, 200 << 10);
    let mut att = Vec::new();
    for i in 0..9 {
        let mut c = Client::with(sw.port, &format!("a{i}"), &format!("?g=a{i}"), Opts::default()).await;
        c.welcome().await;
        att.push(c);
    }
    sleep(100).await;
    let pv = sw.peer_named("Victim").unwrap();
    // each sends 80 KB a second for 5 s: 720 KB/s in all
    let t0 = Instant::now();
    while t0.elapsed().as_millis() < 5000 {
        for a in &att {
            if a.is_open() {
                for _ in 0..2 {
                    a.udp(pv.vip, 50000, 9900, &[0; 8000], 4, 0);
                }
            }
        }
        sleep(200).await;
    }
    sleep(300).await;
    let info = sw.info().await;
    let cut = att.iter().filter(|a| !a.is_open()).count();
    println!(
        "9 clients streaming at a receiver reading about 250 KB/s: receiver {}; senders cut {cut}/9; cuts {}",
        if sw.has(pv.vip) { "still on" } else { "CUT" },
        info["cuts"]
    );
    assert!(sw.has(pv.vip), "the receiver that reads stays");
    assert_eq!(info["cuts"]["slow"], 0);
    assert!(cut >= 1 && info["cuts"]["hog"].as_u64().unwrap() >= 1, "senders cut");
    reading.abort();
    sw.close().await;
}

// the review of F2's fix: a receiver that stopped reading (and kept its connection alive with text pings) got the
// SENDER of an ordinary stream cut
// a receiver that stopped reading: it goes, not the player streaming to it
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_receiver_that_stopped_reading_goes_not_the_player_streaming_to_it() {
    use tokio::io::AsyncWriteExt;
    let _s = serial().await;
    let sw = start(stream_opts()).await;
    let guest = raw_peer(sw.port, "Guest", "?g=guest").await;
    // stops reading, still sends: text pings every 200 ms
    let (_gr, mut gw) = guest.into_split();
    let keep = tokio::spawn(async move {
        loop {
            sleep(200).await;
            if gw.write_all(&frame(0x1, br#"{"t":"ping","id":1}"#)).await.is_err() {
                return;
            }
        }
    });
    let mut host = Client::with(sw.port, "Host", "?g=host", Opts::default()).await;
    host.welcome().await;
    sleep(100).await;
    let pg = sw.peer_named("Guest").unwrap();
    let t0 = Instant::now();
    while t0.elapsed().as_millis() < 5000 && sw.has(pg.vip) && host.is_open() {
        host.udp(pg.vip, 50000, 9900, &[0; 8000], 4, 0);
        host.udp(pg.vip, 50000, 9900, &[0; 8000], 4, 0);
        // 640 KB/s
        sleep(25).await;
    }
    keep.abort();
    let info = sw.info().await;
    println!(
        "host streaming to a guest that stopped reading: host {}, guest {} after {} ms; cuts {}",
        if host.is_open() { "still on" } else { "CUT" },
        if sw.has(pg.vip) { "still on" } else { "cut" },
        t0.elapsed().as_millis(),
        info["cuts"]
    );
    assert!(host.is_open(), "the host stays");
    assert!(!sw.has(pg.vip), "the guest that stopped reading goes");
    assert_eq!(info["cuts"]["hog"], 0);
    host.close().await;
    sw.close().await;
}

// a broadcast costs its size for every player it reaches; one client address shares one budget
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_broadcast_costs_its_size_for_every_player_it_reaches() {
    let _s = serial().await;
    const RATE: usize = 64 << 10;
    let sw = start(LanOptions {
        byte_rate: RATE as f64,
        frame_rate: 100000.0,
        group_byte_rate: RATE as f64,
        group_frame_rate: 100000.0,
        site_byte_rate: RATE as f64,
        site_frame_rate: 100000.0,
        max_peers: 16,
        ..opts()
    })
    .await;
    let mut rx = Vec::new();
    for i in 0..7 {
        let mut c = Client::connect(sw.port, &format!("R{i}")).await;
        c.welcome().await;
        rx.push(c);
    }
    let mut tx = Client::connect(sw.port, "Sender").await;
    tx.welcome().await;
    // 3 s of 1000-byte broadcasts as fast as the sender can
    let t0 = Instant::now();
    while t0.elapsed().as_millis() < 3000 {
        for _ in 0..20 {
            tx.udp(BROADCAST, 1, 2, &[0; 1000 - 16], 1, 0);
        }
        sleep(2).await;
    }
    sleep(300).await;
    let out: usize = rx.iter().map(|c| c.got().frames.iter().map(|f| f.0.len()).sum::<usize>()).sum();
    let secs = t0.elapsed().as_secs_f64();
    println!(
        "broadcast: the switch sent the 7 receivers {} KB in {secs:.1} s for one sender limited to {} KB/s (burst 2 s)",
        out / 1024,
        RATE >> 10
    );
    assert!(
        (out as f64) <= RATE as f64 * (secs + 2.0) * 1.1,
        "the switch sends no more for a sender than its rate, broadcast or not ({out} bytes)"
    );
    // two connections of one client address (one group) share its budget; another address has its own
    let mut g1 = Client::with(sw.port, "G1", "?g=home", Opts::default()).await;
    g1.welcome().await;
    let mut g2 = Client::with(sw.port, "G2", "?g=home", Opts::default()).await;
    g2.welcome().await;
    let mut other = Client::with(sw.port, "Other", "?g=elsewhere", Opts::default()).await;
    other.welcome().await;
    let dst = &rx[0];
    let blast = |c: &Client| {
        for _ in 0..400 {
            c.udp(dst.vip, 1, 2, &[0; 1000 - 16], 1, 0);
        }
    };
    let before = dst.frames();
    blast(&g1);
    blast(&g2);
    sleep(500).await;
    let from_group = dst.got().frames[before..].iter().filter(|f| [g1.vip, g2.vip].contains(&src(&f.0))).count();
    let before2 = dst.frames();
    blast(&other);
    sleep(500).await;
    let from_other = dst.got().frames[before2..].iter().filter(|f| src(&f.0) == other.vip).count();
    println!(
        "one address with two connections got {from_group} frames through, another address alone {from_other} (the budget of one address: {} KB burst)",
        (2 * RATE) >> 10
    );
    assert!(from_group <= (2 * RATE) / 1000 + 20, "two connections of one address share one budget ({from_group})");
    assert!(from_other >= 100, "another address is not affected ({from_other})");
    // the addresses of one site (an IPv6 /48) share the site's budget too
    let mut s1 = Client::with(sw.port, "S1", "?g=s1&s=site", Opts::default()).await;
    s1.welcome().await;
    let mut s2 = Client::with(sw.port, "S2", "?g=s2&s=site", Opts::default()).await;
    s2.welcome().await;
    let before3 = dst.frames();
    blast(&s1);
    blast(&s2);
    sleep(500).await;
    let from_site = dst.got().frames[before3..].iter().filter(|f| [s1.vip, s2.vip].contains(&src(&f.0))).count();
    println!("two addresses of one site got {from_site} frames through (the site's budget: {} KB burst)", (2 * RATE) >> 10);
    assert!(from_site <= (2 * RATE) / 1000 + 20, "one site shares one budget ({from_site})");
    for c in rx.iter().chain([&tx, &g1, &g2, &other, &s1, &s2]) {
        c.close().await;
    }
    sw.close().await;
}

// the most free roam sends goes through one connection: 32 friends at 60 snapshots a second
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_most_free_roam_sends_goes_through_one_connection() {
    let _s = serial().await;
    let sw = start(opts()).await;
    let mut a = Client::connect(sw.port, "Roamer").await;
    a.welcome().await;
    let mut b = Client::connect(sw.port, "Friend").await;
    b.welcome().await;
    // 4 s (past the 2 s burst) of 2000 snapshots of 94 bytes a second: 32 x 60, HELLOs and PINGs
    let t0 = Instant::now();
    let mut sent = 0;
    while t0.elapsed().as_millis() < 4000 {
        let due = (t0.elapsed().as_secs_f64() * 2000.0) as usize;
        while sent < due {
            a.udp(b.vip, 47210, 47210, &[0; 94 - 16], 1, 0);
            sent += 1;
        }
        sleep(5).await;
    }
    sleep(300).await;
    let got = b.frames();
    println!("free roam at the most: {got} of {sent} snapshots through");
    assert!(got as f64 >= sent as f64 * 0.99, "none dropped by the connection's limits ({got} of {sent})");
    a.close().await;
    b.close().await;
    sw.close().await;
}

// with a store, a restart of the server keeps each player's address for its token (and only for it)
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn with_a_store_a_restart_keeps_each_players_address_for_its_token() {
    let _s = serial().await;
    let dir = tempfile::tempdir().unwrap();
    let store = dir.path().join("lan-addresses.json");
    let sw = start_full(opts(), "", None, None, Some(store.clone())).await;
    let mut a = Client::connect(sw.port, "Alice").await;
    let wa = a.welcome().await;
    let mut b = Client::connect(sw.port, "Bob").await;
    let wb = b.welcome().await;
    sleep(600).await;
    let saved = std::fs::read_to_string(&store).unwrap();
    let list: Value = serde_json::from_str(&saved).unwrap();
    assert_eq!(list.as_array().unwrap().len(), 2, "both addresses stored");
    assert!(!saved.contains(wa["token"].as_str().unwrap()) && !saved.contains(wb["token"].as_str().unwrap()), "hashes, not the tokens");
    sw.close().await;
    a.closed().await;
    b.closed().await;
    // the server comes back: an attacker who asks first gets neither address; the players get theirs
    let sw2 = start_full(opts(), "", None, None, Some(store.clone())).await;
    let mut m = Client::new(sw2.port, "Mallory", "", Some((a.vip, wb["token"].as_str().unwrap())), Opts::default()).await;
    let wm = m.welcome().await;
    assert!(![a.vip, b.vip].contains(&vip_of(&wm)), "the attacker got {}", wm["vipStr"]);
    let mut a2 = Client::new(sw2.port, "Alice", "", Some((a.vip, wa["token"].as_str().unwrap())), Opts::default()).await;
    let mut b2 = Client::new(sw2.port, "Bob", "", Some((b.vip, wb["token"].as_str().unwrap())), Opts::default()).await;
    assert_eq!(vip_of(&a2.welcome().await), a.vip);
    assert_eq!(vip_of(&b2.welcome().await), b.vip);
    for c in [&m, &a2, &b2] {
        c.close().await;
    }
    sw2.close().await;
}

// with a store, an address of before the restart that nobody came back for counts as a browser that left for the
// hosts, once, when its reservation ends (the sweep, or the admission of a newcomer); one a departure reserved, they
// heard of at the departure.  (The Online host keeps a race and its players' names until it hears they left.)
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn with_a_store_an_address_nobody_came_back_for_is_gone_for_the_hosts() {
    let _s = serial().await;
    let dir = tempfile::tempdir().unwrap();
    let store = dir.path().join("lan-addresses.json");
    let o = LanOptions { reserve_ms: 400, ..opts() };
    let kept = |n: u8| format!(r#"{{"v":"10.77.{n}.2","h":"{}"}}"#, nfsu2_util::js::base64url(&[n; 32]));
    let (a, b) = (ip("10.77.0.2"), ip("10.77.1.2"));
    let host = || {
        let gone: Arc<Mutex<Vec<u32>>> = Default::default();
        let h = FakeHost { vip: 0x0A4D_0001, name: "online".into(), listed: true, vips: Default::default(), gone: gone.clone() };
        (Arc::new(h), gone)
    };
    // the sweep: the two addresses of before, once; Alice, who dropped after the start, at her departure only
    std::fs::write(&store, format!("[{},{}]", kept(0), kept(1))).unwrap();
    let (online, gone) = host();
    let sw = start_full(o.clone(), "", Some(online), None, Some(store.clone())).await;
    let mut alice = Client::connect(sw.port, "Alice").await;
    alice.welcome().await;
    assert!(![a, b].contains(&alice.vip), "a newcomer gets no address of before");
    assert!(gone.lock().unwrap().is_empty(), "not before their time");
    sw.lan.terminate_all();
    wait_for(2000, || gone.lock().unwrap().len() == 1).await;
    sleep(500).await;
    sw.lan.keepalive();
    sw.lan.keepalive();
    assert_eq!(*gone.lock().unwrap(), [alice.vip, a, b]);
    alice.closed().await;
    sw.close().await;
    // a newcomer's admission: the same, before its welcome
    std::fs::write(&store, format!("[{},{}]", kept(0), kept(1))).unwrap();
    let (online, gone) = host();
    let sw = start_full(o, "", Some(online), None, Some(store.clone())).await;
    sleep(500).await;
    let mut carol = Client::connect(sw.port, "Carol").await;
    carol.welcome().await;
    assert_eq!(*gone.lock().unwrap(), [a, b]);
    carol.close().await;
    sw.close().await;
}

// addresses: 6000 on 10.77.0.0/19, handed out round twenty-four of its /24s; never .0, .1 or .252 and up; full past them
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn addresses_6000_round_the_twenty_four_24s_full_past_them() {
    let _s = serial().await;
    let lan = LanSwitch::new(LanOptions { max_peers: 6000, ..LanOptions::default() }, "", None, None, None, None);
    let mut got = Vec::new();
    let now = nfsu2_util::now_ms();
    for i in 0..6000 {
        let v = lan.allocate();
        assert!(v != 0, "address {i}");
        got.push(v);
        lan.reserve_for_test(v, [0; 32], now + 60_000, Some(&format!("g{i}")), None);
    }
    let first: Vec<String> = got[..18].iter().map(|v| ip_str(*v)).collect();
    let mut want: Vec<String> = (0..16).map(|b| format!("10.77.{b}.2")).collect();
    want.extend(["10.77.16.2".to_string(), "10.77.17.2".to_string()]);
    assert_eq!(first, want);
    assert_eq!(ip_str(got[24]), "10.77.0.3");
    assert_eq!(ip_str(got[5999]), "10.77.23.251");
    assert_eq!(got.iter().collect::<HashSet<_>>().len(), 6000, "all distinct");
    let mut sorted = got.clone();
    sorted.sort();
    let mut all = all_vips();
    all.sort();
    assert_eq!(sorted, all, "every one of them");
    assert_eq!(lan.allocate(), 0, "none left");
    assert!(lan.full());
    lan.stop();
}

// LAN play's broadcasts (kinds 3-5) reach only the browsers with LAN play open; free roam's reach everyone
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn lan_plays_broadcasts_reach_only_the_browsers_with_lan_play_open() {
    let _s = serial().await;
    let sw = start(opts()).await;
    let mut open = Client::with(sw.port, "Open", "", lan(1)).await;
    let mut shut = Client::with(sw.port, "Shut", "", lan(0)).await;
    let mut old = Client::connect(sw.port, "Old").await;
    let mut later = Client::with(sw.port, "Later", "", lan(0)).await;
    let mut tx = Client::with(sw.port, "Host", "", lan(1)).await;
    for c in [&mut open, &mut shut, &mut old, &mut later, &mut tx] {
        c.welcome().await;
    }
    // its hello is in
    for c in [&open, &shut, &old, &later, &tx] {
        c.until_true(|g| g.count("peers") > 0).await;
    }
    tx.udp(BROADCAST, 9999, 9999, b"any LAN games?", 3, 0);
    tx.udp(BROADCAST, 47210, 47210, b"free roam", 1, 0);
    open.until_true(|g| g.frames.len() == 2).await;
    old.until_true(|g| g.frames.len() == 2).await;
    shut.until_true(|g| g.frames.len() == 1).await;
    later.until_true(|g| g.frames.len() == 1).await;
    sleep(50).await;
    let kinds = |c: &Client| -> Vec<u8> {
        let mut k: Vec<u8> = c.got().frames.iter().map(|f| f.0[0]).collect();
        k.sort();
        k
    };
    assert_eq!(kinds(&shut), [1], "LAN play shut: free roam's broadcast only");
    assert_eq!(kinds(&old), [1, 3], "a page of before (no lan in its hello): every broadcast, as before");
    // LAN play opens later ({"t":"lan"}), then shuts again
    later.text(r#"{"t":"lan","on":1}"#);
    sleep(50).await;
    tx.udp(BROADCAST, 9999, 9999, b"again", 5, 0);
    later.until_true(|g| g.frames.len() == 2).await;
    assert_eq!(later.got().frames[1].0[0], 5);
    later.text(r#"{"t":"lan","on":0}"#);
    sleep(50).await;
    tx.udp(BROADCAST, 9999, 9999, b"once more", 3, 0);
    open.until_true(|g| g.frames.len() == 4).await;
    sleep(50).await;
    assert_eq!(later.frames(), 2, "shut again: no more of them");
    assert_eq!(shut.frames(), 1);
    // unicast is not affected
    tx.udp(shut.vip, 9999, 9999, b"to you", 3, 0);
    shut.until_true(|g| g.frames.len() == 2).await;
    for c in [&open, &shut, &old, &later, &tx] {
        c.close().await;
    }
    sw.close().await;
}

// a page that lets go (1000 "idle" or "closed") frees its address at once; a connection lost otherwise keeps it for its
// token
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_page_that_lets_go_frees_its_address_at_once() {
    let _s = serial().await;
    let sw = start(opts()).await;
    let mut cs = Vec::new();
    for n in ["A", "B", "C", "D"] {
        let mut c = Client::connect(sw.port, n).await;
        c.welcome().await;
        cs.push(c);
    }
    cs[0].close_with(1000, "idle");
    cs[1].close_with(1000, "closed");
    cs[2].close_with(1000, "bye");
    cs[3].terminate();
    for c in &cs[..3] {
        c.closed().await;
    }
    wait_for(5000, || sw.peers().is_empty()).await;
    let mut kept = sw.reserved();
    kept.sort();
    let mut want = vec![cs[2].vip, cs[3].vip];
    want.sort();
    assert_eq!(kept, want, "only the ones that did not say so keep their address");
    assert_eq!(sw.info().await["released"], 2);
    sw.close().await;
}

// the server's console says what the players on the switch do (src/app/run.rs, every 5 minutes): each counted once
// breakdown: Online mode, the LAN or Online screens, free roam, an older page, leaving or starting
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn breakdown_online_lan_screens_free_roam_older_page_leaving_or_starting() {
    let _s = serial().await;
    let in_online = Arc::new(Mutex::new(HashSet::new()));
    let in_roam = Arc::new(Mutex::new(HashSet::new()));
    let online = Arc::new(FakeHost { vip: 0x0A4D_0001, name: "online".into(), listed: true, vips: in_online.clone(), gone: Default::default() });
    let roam = Arc::new(FakeHost { vip: 0x0A4D_00FD, name: String::new(), listed: false, vips: in_roam.clone(), gone: Default::default() });
    let sw = start_full(opts(), "", Some(online), Some(roam), None).await;
    let mut all = Vec::new();
    for (name, l) in
        [("Online", Some(1)), ("LanScreens", Some(1)), ("Roamer", Some(0)), ("OldRoamer", None), ("Old", None), ("Leaving", Some(0))]
    {
        let mut c = Client::with(sw.port, name, "", Opts { lan: l, ..Opts::default() }).await;
        c.welcome().await;
        all.push(c);
    }
    for x in &all {
        x.until_true(|g| g.count("peers") > 0).await;
    }
    in_online.lock().unwrap().insert(all[0].vip);
    in_roam.lock().unwrap().extend([all[2].vip, all[3].vip, all[0].vip]);
    assert_eq!(
        sw.lan.breakdown().await,
        Breakdown { players: 6, online: 1, lan: 1, roam: 2, older: 1, other: 1, bundles: 0, ..Breakdown::default() }
    );
    // (the Online host is on the peer list, as { vip, name, server: true }; the hub is on none)
    let list = all[1].got().ctl.iter().rev().find(|m| m["t"] == "peers").cloned().unwrap();
    let last = list["peers"].as_array().unwrap().last().unwrap().clone();
    assert_eq!(last, serde_json::json!({ "vip": 0x0A4D_0001u32, "name": "online", "server": true }));
    for x in &all {
        x.close().await;
    }
    sw.close().await;
}

// ---- bundles (kind 32): several frames in one message ----

// bundles: each entry reaches its destinations as a frame of its own; a client that takes bundles gets one message a
// turn, one that does not gets frames as before
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn bundles_each_entry_reaches_its_destinations_as_a_frame_of_its_own() {
    let _s = serial().await;
    let sw = start(opts()).await;
    let mut a = Client::with(sw.port, "A", "", bundles()).await;
    let mut b = Client::with(sw.port, "B", "", bundles()).await;
    let mut c = Client::connect(sw.port, "C").await;
    let mut d = Client::with(sw.port, "D", "", bundles()).await;
    let wa = a.welcome().await;
    for x in [&mut b, &mut c, &mut d] {
        x.welcome().await;
    }
    assert_eq!(wa["bundle"], 1, "the welcome says the switch takes bundles");
    sleep(100).await;
    // a snapshot to three friends once, one frame to B alone, a LAN broadcast
    a.send_bundle(&[
        (frame_of(0, b"snap", 1, 9999, 9999), vec![b.vip, c.vip, d.vip]),
        (frame_of(b.vip, b"only-b", 1, 9999, 9999), vec![]),
        (frame_of(BROADCAST, b"lan", 3, 9999, 9999), vec![]),
    ]);
    b.until_true(|g| g.dgrams().len() >= 3).await;
    c.until_true(|g| g.dgrams().len() >= 2).await;
    d.until_true(|g| g.dgrams().len() >= 2).await;
    sleep(100).await;
    let got = |x: &Client| -> Vec<String> {
        x.dgrams()
            .iter()
            .map(|g| {
                let to = if g.dst == BROADCAST {
                    "all".to_string()
                } else if g.dst == x.vip {
                    "me".to_string()
                } else {
                    g.dst.to_string()
                };
                format!("{}>{to}", g.data)
            })
            .collect()
    };
    assert_eq!(got(&b), ["snap>me", "only-b>me", "lan>all"], "B: in order, each with its own destination");
    assert_eq!(got(&c), ["snap>me", "lan>all"]);
    assert_eq!(got(&d), ["snap>me", "lan>all"]);
    for x in [&b, &c, &d] {
        assert!(x.dgrams().iter().all(|g| g.src == a.vip), "from A, as the switch wrote it");
    }
    assert_eq!(b.frames(), 1, "B (takes bundles): one message for the three");
    assert_eq!(b.got().frames[0].0[0], 32);
    assert_eq!(c.frames(), 2, "C (a client of before): two frames, as before");
    assert!(c.got().frames.iter().all(|f| f.0[0] != 32));
    assert!(a.dgrams().is_empty(), "nothing back to the sender");
    let i = sw.info().await;
    assert_eq!(i["bundlesIn"], 1);
    assert_eq!(i["bundlesOut"], 2);
    assert_eq!(i["frames"], 5, "five frames routed");
    // what comes in one turn from several senders goes to B as one message
    let n0 = b.frames();
    c.udp(b.vip, 9999, 9999, b"c1", 1, 0);
    d.udp(b.vip, 9999, 9999, b"d1", 1, 0);
    a.udp(b.vip, 9999, 9999, b"a1", 1, 0);
    b.until_true(|g| g.dgrams().len() >= 6).await;
    let mut last: Vec<String> = b.dgrams()[3..].iter().map(|g| g.data.clone()).collect();
    last.sort();
    assert_eq!(last, ["a1", "c1", "d1"]);
    println!("three senders to B: {} message(s)", b.frames() - n0);
    for x in [&a, &b, &c, &d] {
        x.close().await;
    }
    sw.close().await;
}

// bundles: LAN play's datagrams (a race's car updates) go to a client that takes bundles at once; free roam's wait for
// the bundle clock's tick
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn bundles_lan_play_datagrams_do_not_wait_for_the_clock() {
    let _s = serial().await;
    // a fixed tick of 400 ms: 8 datagrams 50 ms apart that wait for it wait 350 ms or more, one of them at least (no
    // probes: a ping wakes the writer, which takes the datagrams queued along)
    let sw = start(LanOptions { bundle_ms: 400, bundle_max_ms: 400, probe_ms: 0, ..opts() }).await;
    let mut a = Client::connect(sw.port, "A").await;
    let mut b = Client::with(sw.port, "B", "", bundles()).await;
    a.welcome().await;
    b.welcome().await;
    sleep(100).await;
    // ms from sending to B's getting it, for each of 8 datagrams of this kind
    async fn waits(a: &Client, b: &Client, kind: u8) -> Vec<u64> {
        let mut sent = Vec::new();
        for i in 0..8 {
            let tag = format!("k{kind}-{i}");
            sent.push((tag.clone(), Instant::now()));
            a.udp(b.vip, 3658, 3658, tag.as_bytes(), kind, 0);
            sleep(50).await;
        }
        let pre = format!("k{kind}-");
        b.until_true(|g| g.dgrams().iter().filter(|d| d.data.starts_with(&pre)).count() == 8).await;
        let g = b.got();
        let got = g.dgrams();
        sent.iter()
            .map(|(tag, t)| {
                let d = got.iter().find(|d| &d.data == tag).unwrap();
                g.frames[d.msg].1.duration_since(*t).as_millis() as u64
            })
            .collect()
    }
    let lan = waits(&a, &b, 3).await;
    let roam = waits(&a, &b, 1).await;
    println!("LAN play: {lan:?} ms; free roam: {roam:?} ms");
    assert!(lan.iter().all(|&ms| ms < 150), "LAN play's go at once: {lan:?}");
    assert!(roam.iter().any(|&ms| ms >= 300), "free roam's wait for the tick: {roam:?}");
    a.close().await;
    b.close().await;
    sw.close().await;
}

// bundles: each frame pays the budgets as a frame of its own; a broken entry, one over the budget, or a bundle too
// large ends it
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn bundles_each_frame_pays_the_budgets_a_broken_entry_ends_it() {
    let _s = serial().await;
    let sw = start(LanOptions { frame_rate: 10.0, ..opts() }).await;
    let mut a = Client::with(sw.port, "A", "?g=a", Opts::default()).await;
    a.welcome().await;
    let mut b = Client::with(sw.port, "B", "?g=b", Opts::default()).await;
    b.welcome().await;
    sleep(50).await;
    // 30 times to B in one entry: the burst (2 s of 10 a second) gets through, the rest and what follows do not
    a.send_bundle(&[(frame_of(0, b"x", 1, 9999, 9999), vec![b.vip; 30]), (frame_of(b.vip, b"after", 1, 9999, 9999), vec![])]);
    sleep(300).await;
    assert_eq!(b.dgrams().len(), 20);
    assert!(b.dgrams().iter().all(|g| g.data == "x"));
    assert!(sw.info().await["limited"].as_u64().unwrap() >= 1);
    sw.close().await;
    let sw2 = start(opts()).await;
    let mut a = Client::connect(sw2.port, "A").await;
    a.welcome().await;
    let mut b = Client::connect(sw2.port, "B").await;
    b.welcome().await;
    sleep(50).await;
    let good = bundle_of(&[(frame_of(b.vip, b"one", 1, 9999, 9999), vec![])]);
    // an entry longer than what is left: the ones before it went, nothing after
    let mut broken = good.clone();
    broken.extend_from_slice(&[0, 0, 0x10, 0x00]);
    broken.extend_from_slice(&frame_of(b.vip, b"cut off", 1, 9999, 9999));
    a.binary(broken);
    // a bundle in a bundle, a bundle over maxBundle (64 KB)
    let mut nested = vec![32, 0, 0, 0];
    nested.extend_from_slice(&frame_of(b.vip, b"nested", 1, 9999, 9999)[4..]);
    a.send_bundle(&[(nested, vec![])]);
    let big: Vec<(Vec<u8>, Vec<u32>)> = (0..9).map(|_| (frame_of(b.vip, "z".repeat(8000).as_bytes(), 1, 9999, 9999), vec![])).collect();
    a.binary(bundle_of(&big));
    sleep(300).await;
    assert_eq!(b.dgrams().iter().map(|g| g.data.as_str()).collect::<Vec<_>>(), ["one"]);
    let limited = sw2.info().await["limited"].as_u64().unwrap();
    assert!(limited >= 3, "limited {limited}");
    a.udp(b.vip, 9999, 9999, b"still fine", 1, 0);
    b.until_true(|g| g.dgrams().len() == 2).await;
    sw2.close().await;
}

/// A player on a plain socket that answers the switch's pings `delay` ms late (its path's queues), and counts the
/// datagrams it gets by their free-roam message type (byte 21 of the frame: 1 HELLO, 3 SNAP).
struct Laggy {
    vip: u32,
    delay: Arc<AtomicU64>,
    /// the pongs waiting for their time: when, and the ping's payload
    due: Arc<Mutex<VecDeque<(Instant, Vec<u8>)>>>,
    counts: Arc<Mutex<[usize; 16]>>,
    tasks: Vec<JoinHandle<()>>,
}

/// a frame from the switch (never masked): its opcode, its payload, and the bytes it took
fn server_frame(b: &[u8]) -> Option<(u8, Vec<u8>, usize)> {
    if b.len() < 2 {
        return None;
    }
    let (len, at) = match b[1] & 0x7F {
        126 if b.len() >= 4 => (u16::from_be_bytes([b[2], b[3]]) as usize, 4),
        127 if b.len() >= 10 => (u64::from_be_bytes(b[2..10].try_into().unwrap()) as usize, 10),
        126 | 127 => return None,
        n => (n as usize, 2),
    };
    (b.len() >= at + len).then(|| (b[0] & 0x0F, b[at..at + len].to_vec(), at + len))
}

impl Laggy {
    async fn start(port: u16, name: &str, delay_ms: u64) -> Laggy {
        let (mut r, mut w) = raw_peer(port, name, "").await.into_split();
        let delay = Arc::new(AtomicU64::new(delay_ms));
        let due: Arc<Mutex<VecDeque<(Instant, Vec<u8>)>>> = Arc::default();
        let counts: Arc<Mutex<[usize; 16]>> = Arc::default();
        let (vtx, vrx) = tokio::sync::oneshot::channel();
        let (d, q, c) = (delay.clone(), due.clone(), counts.clone());
        let reader = tokio::spawn(async move {
            let mut vtx = Some(vtx);
            let mut buf = Vec::new();
            let mut chunk = vec![0u8; 64 << 10];
            loop {
                match r.read(&mut chunk).await {
                    Ok(0) | Err(_) => return,
                    Ok(n) => buf.extend_from_slice(&chunk[..n]),
                }
                while let Some((op, payload, used)) = server_frame(&buf) {
                    buf.drain(..used);
                    match op {
                        0x1 => {
                            let m: Value = serde_json::from_slice(&payload).unwrap();
                            if m["t"] == "welcome" {
                                if let Some(t) = vtx.take() {
                                    let _ = t.send(vip_of(&m));
                                }
                            }
                        }
                        0x2 if payload.len() > 21 => c.lock().unwrap()[payload[21] as usize & 15] += 1,
                        0x9 => {
                            let at = Instant::now() + Duration::from_millis(d.load(Relaxed));
                            q.lock().unwrap().push_back((at, payload));
                        }
                        _ => {}
                    }
                }
            }
        });
        let q = due.clone();
        let writer = tokio::spawn(async move {
            loop {
                let ready: Vec<Vec<u8>> = {
                    let mut q = q.lock().unwrap();
                    let mut v = Vec::new();
                    while q.front().is_some_and(|(at, _)| *at <= Instant::now()) {
                        v.push(q.pop_front().unwrap().1);
                    }
                    v
                };
                for p in ready {
                    if w.write_all(&frame(PONG, &p)).await.is_err() {
                        return;
                    }
                }
                sleep(5).await;
            }
        });
        let vip = tokio::time::timeout(Duration::from_secs(5), vrx).await.expect("a welcome").unwrap();
        Laggy { vip, delay, due, counts, tasks: vec![reader, writer] }
    }

    /// its path keeps up from now on: pings answered at once, the late answers still waiting never sent
    fn keep_up(&self) {
        self.delay.store(0, Relaxed);
        self.due.lock().unwrap().clear();
    }

    /// what it got since the last call
    fn take(&self) -> [usize; 16] {
        std::mem::take(&mut *self.counts.lock().unwrap())
    }
}

impl Drop for Laggy {
    fn drop(&mut self) {
        for t in &self.tasks {
            t.abort();
        }
    }
}

/// a free-roam datagram as roam_core sends it, far enough for the switch: "N2FR", version 1, its type
fn roam_dgram(kind: u8) -> Vec<u8> {
    let mut d = b"N2FR".to_vec();
    d.extend_from_slice(&[1, kind, 0, 0]);
    d.extend_from_slice(&[0; 40]);
    d
}

/// until the breakdown says `f`, at most `secs`
async fn until_breakdown(sw: &Switch, secs: u64, what: &str, f: impl Fn(&Breakdown) -> bool) -> Breakdown {
    let t0 = Instant::now();
    loop {
        let b = sw.lan.breakdown().await;
        if f(&b) {
            return b;
        }
        assert!(t0.elapsed().as_secs() < secs, "{what}: {b:?}");
        sleep(50).await;
    }
}

// a player behind on its path (its pings answered 2.5 s late) gets a share of free roam's snapshots and every other
// message, and all of them again once it keeps up (lag.rs)
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_player_behind_gets_a_share_of_the_snapshots_and_all_again_once_it_keeps_up() {
    let _s = serial().await;
    let sw = start(LanOptions { probe_ms: 100, ..unlimited() }).await;
    let laggy = Laggy::start(sw.port, "Laggy", 2500).await;
    let mut s = Client::connect(sw.port, "Sender").await;
    s.welcome().await;
    let (snap, hello) = (roam_dgram(3), roam_dgram(1));
    let send = |n: usize| {
        for i in 0..n {
            s.udp(laggy.vip, 47210, 47210, if i % 5 == 4 { &hello } else { &snap }, 1, 0);
        }
    };
    // before its first round trip it gets all of them
    send(100);
    sleep(300).await;
    assert_eq!(laggy.take()[3], 80);
    // its round trips 2.5 s: behind, and the least share at once
    let b = until_breakdown(&sw, 6, "thinned", |b| b.thinning == 1).await;
    assert_eq!(b.behind, 1, "{b:?}");
    send(500);
    sleep(300).await;
    let got = laggy.take();
    assert_eq!(got[1], 100, "every HELLO");
    assert!(got[3] < 80, "a share of the 400 snapshots ({})", got[3]);
    assert!(sw.info().await["thinned"].as_u64().unwrap() >= 320);
    assert_eq!(sw.info().await["shed"], 0, "nothing for a long queue: it reads everything");
    // its round trips back to the path's own: all of them again, an eighth more a round trip
    laggy.keep_up();
    let b = until_breakdown(&sw, 6, "all again", |b| b.thinning == 0).await;
    assert_eq!(b.behind, 0, "{b:?}");
    let thinned = sw.info().await["thinned"].as_u64().unwrap();
    send(500);
    sleep(300).await;
    let got = laggy.take();
    assert_eq!((got[3], got[1]), (400, 100));
    assert_eq!(sw.info().await["thinned"].as_u64().unwrap(), thinned);
    s.close().await;
    drop(laggy);
    sw.close().await;
}

// compact bundles: a page that takes them (its hello's "compact":1) gets the same frames as one that does not, byte for
// byte, in about a quarter fewer bytes; a client cannot send one (it could name any source) (compact.rs)
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn compact_bundles_carry_the_same_frames_in_fewer_bytes() {
    let _s = serial().await;
    let sw = start(unlimited()).await;
    let mut full = Client::with(sw.port, "Full", "", bundles()).await;
    full.welcome().await;
    let mut small = Client::with(sw.port, "Small", "", Opts { bundle: true, compact: true, ..Opts::default() }).await;
    small.welcome().await;
    let mut s = Client::connect(sw.port, "Sender").await;
    s.welcome().await;
    sleep(100).await;
    let mut broken = roam_datagram(3, 70);
    *broken.last_mut().unwrap() ^= 0x55;
    let both = vec![full.vip, small.vip];
    let frames = [
        // free roam: snapshots to both, a HELLO broadcast (discovery), one on other ports, one whose CRC is wrong
        (frame_of(0, &roam_datagram(3, 70), 1, 47210, 47210), both.clone()),
        (frame_of(0, &roam_datagram(3, 82), 1, 47210, 47210), both.clone()),
        (frame_of(BROADCAST, &roam_datagram(1, 40), 1, 47210, 47210), vec![]),
        (frame_of(0, &roam_datagram(3, 70), 1, 47211, 47300), both.clone()),
        (frame_of(0, &broken, 1, 47210, 47210), both.clone()),
        // not free roam's: LAN play's UDP, some other bytes
        (frame_of(0, &[1, 64, 0, 0, 0, 0, 0, 7, 42, 42, 42], 3, 6112, 6112), both.clone()),
        (frame_of(0, b"just bytes", 1, 47210, 47210), both.clone()),
    ];
    for _ in 0..20 {
        s.send_bundle(&frames);
    }
    let want = 20 * 7;
    full.until_true(|g| g.dgrams().len() >= want).await;
    let small_frames = || -> Vec<Vec<u8>> {
        small.got().frames.iter().flat_map(|(b, _)| if b[0] == 33 { unpack_compact(b, small.vip) } else { vec![b.clone()] }).collect()
    };
    let t0 = Instant::now();
    while small_frames().len() < want {
        assert!(t0.elapsed().as_secs() < 5, "{} of {want} frames", small_frames().len());
        sleep(20).await;
    }
    // the same frames, in the same order: to each its own address, the rest byte for byte
    let mine = |f: &[u8], vip: u32| {
        let mut f = f.to_vec();
        if f[8..12] == vip.to_be_bytes() {
            f[8..12].copy_from_slice(&[0, 0, 0, 0]);
        }
        f
    };
    let a: Vec<Vec<u8>> = full.dgrams().iter().map(|d| mine(&d.buf, full.vip)).collect();
    let b: Vec<Vec<u8>> = small_frames().iter().map(|f| mine(f, small.vip)).collect();
    assert_eq!(a.len(), b.len());
    assert!(a == b, "the same frames");
    assert!(small.got().frames.iter().all(|(m, _)| m[0] == 33), "all of them compact");
    let bytes = |c: &Client| c.got().frames.iter().map(|(m, _)| m.len()).sum::<usize>();
    let (bf, bs) = (bytes(&full), bytes(&small));
    assert!(bs * 100 < bf * 85, "fewer bytes: {bs} against {bf}");
    // a client's compact bundle, alone or in a bundle, reaches nobody
    let before = (small.got().frames.len(), sw.info().await["limited"].as_u64().unwrap());
    let mut fake = vec![33u8, 0, 0, 0, 0x01, 0x01, 0x02, 0x00, 0x04];
    fake.extend_from_slice(b"evil");
    s.binary(frame_of(small.vip, &fake, 33, 47210, 47210));
    s.send_bundle(&[(frame_of(small.vip, &fake, 33, 47210, 47210), vec![])]);
    s.udp(small.vip, 47210, 47210, b"after", 1, 0);
    let t0 = Instant::now();
    while small_frames().last().is_none_or(|f| &f[16..] != b"after") {
        assert!(t0.elapsed().as_secs() < 5, "the frame after");
        sleep(20).await;
    }
    assert_eq!(small.got().frames.len(), before.0 + 1, "only the frame after");
    assert_eq!(sw.info().await["limited"].as_u64().unwrap(), before.1 + 2);
    s.close().await;
    full.close().await;
    small.close().await;
    sw.close().await;
}

// a frame of 12 to 15 bytes (the reader takes it: no ports, no data) reaches a page that takes compact bundles as it
// is, and the page stays connected (the third audit: one such broadcast panicked every compact page's writer)
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_frame_without_ports_reaches_a_compact_page_whole() {
    let _s = serial().await;
    let sw = start(unlimited()).await;
    let mut small = Client::with(sw.port, "Small", "", Opts { bundle: true, compact: true, ..Opts::default() }).await;
    small.welcome().await;
    let mut s = Client::connect(sw.port, "Sender").await;
    s.welcome().await;
    sleep(100).await;
    let short = |kind: u8, dst: u32, len: usize| {
        let mut f = vec![kind, 0, 0, 0, 0, 0, 0, 0];
        f.extend_from_slice(&dst.to_be_bytes());
        f.resize(len, 0xAB);
        f
    };
    s.binary(short(1, BROADCAST, 12));
    s.binary(short(1, small.vip, 15));
    s.send_bundle(&[(short(3, BROADCAST, 13), vec![]), (short(5, small.vip, 14), vec![])]);
    s.udp(small.vip, 47210, 47210, b"after", 1, 0);
    let frames = || -> Vec<Vec<u8>> {
        small.got().frames.iter().flat_map(|(b, _)| if b[0] == 33 { unpack_compact(b, small.vip) } else { vec![b.clone()] }).collect()
    };
    let t0 = Instant::now();
    while frames().last().is_none_or(|f| f.len() < 16 || &f[16..] != b"after") {
        assert!(t0.elapsed().as_secs() < 5, "the frame after: {:?}", frames());
        sleep(20).await;
    }
    let got = frames();
    assert_eq!(got.len(), 5, "{got:?}");
    for (f, (kind, len)) in got.iter().zip([(1u8, 12usize), (1, 15), (3, 13), (5, 14)]) {
        assert_eq!((f[0], f.len()), (kind, len));
        assert_eq!(f[4..8], s.vip.to_be_bytes(), "from the sender");
        assert!(f[12..].iter().all(|&b| b == 0xAB), "its bytes as they were");
    }
    s.close().await;
    small.close().await;
    sw.close().await;
}

// the peer list follows LAN play: a page with it shut gets the server's own hosts only and no changes, the whole
// list when it opens LAN play and the changes after that, none again once it shuts it
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn the_peer_list_goes_only_to_pages_with_lan_play_open() {
    let _s = serial().await;
    let sw = start(opts()).await;
    let with = |on: u8| Opts { deltas: true, lan: Some(on), ..Opts::default() };
    let mut shut = Client::with(sw.port, "Shut", "", with(0)).await;
    shut.welcome().await;
    let mut open = Client::with(sw.port, "Open", "", with(1)).await;
    open.welcome().await;
    open.until_true(|g| g.names() == "Open,Shut").await;
    shut.until_true(|g| g.count("peers") == 1).await;
    assert_eq!(shut.got().names(), "", "the hosts only (none on this switch)");
    let mut late = Client::with(sw.port, "Late", "", with(1)).await;
    late.welcome().await;
    open.until_true(|g| g.names() == "Late,Open,Shut").await;
    sleep(400).await;
    {
        let g = shut.got();
        assert_eq!((g.count("delta"), g.names()), (0, String::new()), "no changes while LAN play is shut");
    }
    // LAN play opens: the whole list, then the changes
    shut.text(r#"{"t":"lan","on":1}"#);
    shut.until_true(|g| g.names() == "Late,Open,Shut").await;
    let mut more = Client::with(sw.port, "More", "", with(0)).await;
    more.welcome().await;
    shut.until_true(|g| g.names() == "Late,More,Open,Shut").await;
    // shut again: no more changes
    shut.text(r#"{"t":"lan","on":0}"#);
    sleep(100).await;
    let deltas = shut.got().count("delta");
    let mut last = Client::with(sw.port, "Last", "", with(1)).await;
    last.welcome().await;
    open.until_true(|g| g.names() == "Last,Late,More,Open,Shut").await;
    sleep(400).await;
    assert_eq!(shut.got().count("delta"), deltas);
    // a page of before (no word on LAN play) gets everything, as it did
    let mut old = Client::with(sw.port, "Old", "", deltas_only()).await;
    old.welcome().await;
    old.until_true(|g| g.names() == "Last,Late,More,Old,Open,Shut").await;
    sw.close().await;
}

fn deltas_only() -> Opts {
    Opts { deltas: true, ..Opts::default() }
}
