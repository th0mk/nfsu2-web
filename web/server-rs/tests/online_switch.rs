//! The switch (nfsu2-lan) with the Online host at 10.77.0.1 (nfsu2-online), and browsers on its WebSocket: the two
//! together.
//!   cargo test --test online_switch
//! - the host is on every peer list as server: true, frames to it never reach a browser, its answers do, broadcasts
//!   never reach it, a browser that leaves is reported
//! - ONL-2: what the host sends because of a client's frame is charged to that client (over its budget: that stream
//!   goes, the others stay); a receiver far behind loses its online stream, not its connection

mod common;

use std::collections::HashSet;
use std::sync::{Arc, Mutex};

use bytes::Bytes;
use common::*;
use nfsu2_lan::LanOptions;
use nfsu2_online::{VHOST_NAME, VHOST_VIP};
use nfsu2_util::{FrameCtx, Host, SendResult};
use serde_json::{Value, json};
use tokio::sync::oneshot;

/// a stand-in for the online host: it keeps what the switch hands it
#[derive(Default)]
struct FakeOnline {
    seen: Mutex<Vec<(u32, Vec<u8>)>>,
    gone: Mutex<Vec<u32>>,
}

impl Host for FakeOnline {
    fn vip(&self) -> u32 {
        VHOST_VIP
    }
    fn name(&self) -> &str {
        VHOST_NAME
    }
    fn listed(&self) -> bool {
        true
    }
    fn frame(&self, from: u32, data: Bytes, _ctx: FrameCtx) {
        self.seen.lock().unwrap().push((from, data.to_vec()));
    }
    fn peer_gone(&self, vip: u32) {
        self.gone.lock().unwrap().push(vip);
    }
    fn info(&self) -> oneshot::Receiver<Value> {
        let (tx, rx) = oneshot::channel();
        let _ = tx.send(json!({}));
        rx
    }
    fn vips(&self) -> oneshot::Receiver<HashSet<u32>> {
        let (tx, rx) = oneshot::channel();
        let _ = tx.send(HashSet::new());
        rx
    }
    fn stop(&self) {}
}

// the online host on the peer list, frames to it and back, no broadcasts
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn switch_the_online_host_on_the_peer_list_frames_to_it_and_back_no_broadcasts() {
    let online = Arc::new(FakeOnline::default());
    let s = start_with(LanOptions::default(), Some(online.clone() as Arc<dyn Host>), None).await;
    let send = s.lan.switch_send();
    let a = Browser::open(s.port, "", None).await;
    let b = Browser::open(s.port, "", None).await;
    let peers = a.until(|g| g.peers().filter(|l| l.len() == 3)).await;
    let servers: Vec<(u64, &str)> =
        peers.iter().filter(|p| p["server"] == true).map(|p| (p["vip"].as_u64().unwrap(), p["name"].as_str().unwrap())).collect();
    assert_eq!(servers, [(VHOST_VIP as u64, "NFSU2 Online")]);
    assert!(peers.iter().filter(|p| p["server"] != true).all(|p| p["vip"] != VHOST_VIP));
    // a frame to 10.77.0.1, with a forged source: the online host gets it with the sender's own address
    a.binary(lan_frame(4, 0x0a4d_00fe, VHOST_VIP, 50000, 20920, SYN, 5, &[]));
    assert!(wait_for(2000, || !online.seen.lock().unwrap().is_empty()).await);
    sleep(100).await;
    {
        let seen = online.seen.lock().unwrap();
        assert_eq!(seen.len(), 1);
        assert_eq!(seen[0].0, a.vip);
        assert_eq!(be32(&seen[0].1, 4), a.vip);
    }
    assert_eq!(b.got().frames.len(), 0, "no browser sees it");
    // a broadcast never reaches the online host
    a.binary(lan_frame(3, 0, 0xffff_ffff, 9999, 9999, 0, 0, b"advert"));
    b.until(|g| (g.frames.len() == 1).then_some(())).await;
    assert_eq!(online.seen.lock().unwrap().len(), 1);
    // its answer reaches the browser
    assert_eq!(send(a.vip, Bytes::from(lan_frame(4, VHOST_VIP, a.vip, 20920, 50000, SYNACK, 5, &[])), None), SendResult::Sent);
    let f = a.until(|g| g.frames.iter().find(|x| x[16] == SYNACK).cloned()).await;
    assert_eq!(be32(&f, 4), VHOST_VIP);
    assert_eq!(
        send(ip("10.77.0.200"), Bytes::from(lan_frame(4, VHOST_VIP, 0, 1, 1, RST, 0, &[])), None),
        SendResult::Gone,
        "nobody there (null: not on the switch)"
    );
    // a browser that leaves is reported
    b.close().await;
    assert!(wait_for(2000, || !online.gone.lock().unwrap().is_empty()).await);
    sleep(100).await;
    assert_eq!(*online.gone.lock().unwrap(), [b.vip]);
    a.close().await;
    s.close().await;
}

/// a browser of client `group`, logged in to the lobby as `name`, then moved to the Lounge
async fn in_the_lounge(port: u16, group: &str, name: &str, rcvbuf: Option<u32>) -> (Browser, Conn) {
    let b = Browser::open(port, &format!("?g={group}"), rcvbuf).await;
    let c = b.login(name).await;
    b.send_msg(c, "move", &[("NAME", "Lounge".into())]);
    assert!(b.try_until(3000, |g| g.tcp[&c.port].last("move").map(|_| ())).await.is_some(), "{name} in the Lounge");
    (b, c)
}

// ONL-2, its first half: what the host sends for a client is charged to it (over its budget: that stream goes)
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn onl2_the_switch_what_the_host_sends_for_a_client_is_charged_to_it() {
    let (online, _lines) = online_host();
    // budgets small enough that one big car description to a room of five is more than a client's share
    let o = LanOptions {
        byte_rate: (4 << 10) as f64,
        group_byte_rate: (4 << 10) as f64,
        site_byte_rate: (1 << 20) as f64,
        max_payload: 1 << 20,
        ..LanOptions::default()
    };
    let sw = start(o, Some(online.clone()), None).await;
    let mut victims = Vec::new();
    for i in 0..5 {
        victims.push(in_the_lounge(sw.port, &format!("v{i}"), &format!("Victim{i}"), None).await);
    }
    // the victims' budgets fill up again
    sleep(2500).await;
    let (mal, mc) = in_the_lounge(sw.port, "mal", "Mallory", None).await;
    sleep(2500).await;
    let limited = sw.info().await["limited"].as_u64().unwrap();
    mal.send_msg(mc, "auxi", &[("TEXT", format!("C={}", "A".repeat(2000)))]);
    assert!(
        mal.try_until(2000, |g| g.tcp[&mc.port].reset.then_some(())).await.is_some(),
        "the client whose message made the server send more than its share loses that stream"
    );
    assert!(mal.is_open(), "... not its whole connection");
    for (v, c) in &victims {
        assert!(!v.tcp(*c, |t| t.reset) && v.is_open(), "the others keep theirs");
    }
    let info = sw.info().await;
    assert_eq!(info["limited"].as_u64().unwrap(), limited + 1, "the overrun counted once in the switch's limited");
    assert_eq!(info["online"]["overrun"], 1, "{}", info["online"]);
    sw.close().await;
}

// ONL-2, its second half: a receiver far behind loses its online stream, not its connection.  The receiver truly
// stops reading (small kernel buffers: its SO_RCVBUF and the switch's SO_SNDBUF, as the lan tests keep them) and
// another browser's LAN datagrams fill its queue on the switch until it is just past `send_buffer` (2 MiB here;
// datagrams queue up to `shed_queue`, 4 MiB here), past `online_queue` (1 MiB) too, so that only the order of the
// two checks keeps the connection.  That flood needs budgets that let it through, so this half runs on a switch of
// its own.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn onl2_the_switch_a_receiver_far_behind_loses_its_online_stream_not_its_connection() {
    let (online, _lines) = online_host();
    let send_buffer = 2 << 20;
    let o = LanOptions {
        frame_rate: 100_000.0,
        byte_rate: (64 << 20) as f64,
        group_frame_rate: 100_000.0,
        group_byte_rate: (64 << 20) as f64,
        send_buffer,
        shed_queue: 4 << 20,
        send_buffer_os: 32 << 10,
        max_payload: 1 << 20,
        ..LanOptions::default()
    };
    let online_queue = LanOptions::default().online_queue;
    let sw = start(o, Some(online.clone()), None).await;
    let mut victims = Vec::new();
    for i in 0..5 {
        victims.push(in_the_lounge(sw.port, &format!("v{i}"), &format!("Victim{i}"), if i == 0 { Some(8 << 10) } else { None }).await);
    }
    let slow = &victims[0].0;
    let personas = |info: &Value| info["online"]["personas"].as_u64().unwrap();
    assert_eq!(personas(&sw.info().await), 5);
    // the slow receiver stops reading; a flood of LAN play's datagrams to it, in batches while its queue is short,
    // then one at a time until the queue is past `send_buffer` (and stays so: before the kernel's buffers are full an
    // ACK still frees some of it a moment later), so that no datagram of the flood finds it over the limit
    slow.pause(true);
    let flood = Browser::open(sw.port, "?g=flood", None).await;
    let mut n = 0;
    while n < 4000 {
        let q = sw.buffered(slow.vip);
        if q > send_buffer {
            sleep(100).await;
            if sw.buffered(slow.vip) > send_buffer {
                break;
            }
            continue;
        }
        let batch = if q + (128 << 10) < send_buffer { 8 } else { 1 };
        for _ in 0..batch {
            flood.binary(udp(slow.vip, 9999, 9999, 3, &[0x55; 8000]));
        }
        n += batch;
        flood.flush().await;
        sleep(if batch > 1 { 2 } else { 20 }).await;
    }
    let q = sw.buffered(slow.vip);
    println!("the receiver's queue: {q} bytes after {n} datagrams");
    assert!(q > send_buffer && q > online_queue, "the receiver is far behind ({q} bytes queued after {n} datagrams)");
    assert_eq!(sw.info().await["cuts"]["slow"], 0, "the flood itself cut nobody");
    victims[1].0.send_msg(victims[1].1, "mesg", &[("TEXT", "hello room".into())]);
    // the others heard the message (the host has handled it)
    for (v, c) in &victims[1..] {
        assert!(v.try_until(3000, |g| g.tcp[&c.port].last("+msg").map(|_| ())).await.is_some(), "the room heard it");
        assert!(!v.tcp(*c, |t| t.reset));
    }
    let info = sw.info().await;
    assert!(sw.peer(slow.vip).is_some_and(|p| p.open), "the slow receiver is still on the switch");
    assert_eq!(info["cuts"]["slow"], 0, "no connection was cut for it: {}", info["cuts"]);
    let vips = online.vips().await.unwrap();
    assert!(!vips.contains(&slow.vip), "its lobby session went");
    assert_eq!(personas(&info), 4, "{}", info["online"]);
    assert!(info["online"]["slow"].as_u64().unwrap() >= 1, "{}", info["online"]);
    // once it reads again it has all of what was queued, on the same connection
    slow.pause(false);
    assert!(wait_for(5000, || sw.buffered(slow.vip) == 0).await, "its queue drains");
    assert!(slow.is_open() && sw.peer(slow.vip).is_some(), "its connection stayed");
    sw.close().await;
}

// (a measurement: cargo test --test online_switch -- --ignored --nocapture; NFSU2_FLOOD_CONNS connections, 32 by
// default)  Clients within their budgets (2500 frames a second each, one client address each) send the Online host
// SYNs to a closed port (its cheapest frame: one RST back), in bundles of 250 every 100 ms.
// Every 500 ms: how long the host takes to answer info() (what is queued in front of it), what it got through, and
// this process's memory.
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
#[ignore]
async fn measure_the_online_hosts_queue_under_a_flood_of_frames() {
    let k: usize = std::env::var("NFSU2_FLOOD_CONNS").ok().and_then(|v| v.parse().ok()).unwrap_or(32);
    let secs: u64 = std::env::var("NFSU2_FLOOD_SECS").ok().and_then(|v| v.parse().ok()).unwrap_or(4);
    let (online, _lines) = online_host();
    let sw = start(LanOptions { max_peers: 2000, max_payload: 1 << 20, ..LanOptions::default() }, Some(online.clone()), None).await;
    let mut floods = Vec::new();
    for i in 0..k {
        let b = Browser::open(sw.port, &format!("?g=flood{i}"), None).await;
        b.got().drop_frames = true;
        floods.push(b);
    }
    let rss0 = rss();
    let t0 = std::time::Instant::now();
    let stop = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
    let mut tasks = Vec::new();
    for b in floods.iter() {
        let syns: Vec<Vec<u8>> = (0..250).map(|i| lan_frame(4, 0, VHOST_VIP, 40000 + i, 1, SYN, i as u32, &[])).collect();
        let bundle = bundle_of(&syns);
        let tx = b.sender();
        let stop = stop.clone();
        tasks.push(tokio::spawn(async move {
            let mut iv = tokio::time::interval(std::time::Duration::from_millis(100));
            while !stop.load(std::sync::atomic::Ordering::Relaxed) {
                iv.tick().await;
                tx(bundle.clone());
            }
        }));
    }
    let mut worst = 0.0f64;
    for _ in 0..secs * 2 {
        sleep(500).await;
        let t = std::time::Instant::now();
        let info = online.info().await.unwrap();
        let ms = t.elapsed().as_secs_f64() * 1000.0;
        worst = worst.max(ms);
        let routed = sw.info().await["frames"].as_u64().unwrap();
        println!(
            "{:5.1} s: info() answered in {ms:7.1} ms; frames routed {routed}, SYNs the host took {}; memory +{} MB",
            t0.elapsed().as_secs_f64(),
            info["synIn"],
            (rss() as i64 - rss0 as i64) / (1 << 20)
        );
    }
    stop.store(true, std::sync::atomic::Ordering::Relaxed);
    for t in tasks {
        let _ = t.await;
    }
    let t = std::time::Instant::now();
    let info = online.info().await.unwrap();
    println!(
        "{k} clients, {} frames a second offered: the host's queue drained {:.0} ms after the flood (worst wait {worst:.0} ms), {} SYNs",
        k * 2500,
        t.elapsed().as_secs_f64() * 1000.0,
        info["synIn"]
    );
    sw.close().await;
}

// (a measurement: cargo test --test online_switch -- --ignored --nocapture) The Online host's queue has no bound.
// Its thread made slow here by hand (the dev trace's tap takes 100 us a frame: a thread that handles 10,000 frames a
// second, as a stalled disk or a busy core would make it), 16 clients within their budgets send it SYNs to a closed
// port for 3 s.  What waits for the thread grows as long as they send (the free-roam hub drops frames past
// MAX_PENDING for this; Node could not fall behind: the host ran in the switch's own calls, and every socket waited
// for it): info() answers later and later, the memory grows, and once they stop it takes the thread seconds to
// catch up, while every Online player's lobby is that far behind.  (A first version of this measurement waited for
// each info() while the clients went on sending: it reached 3.8 GB in 10 minutes.)
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
#[ignore]
async fn measure_the_online_hosts_queue_behind_a_slow_thread() {
    use std::sync::Arc;
    use std::sync::atomic::{AtomicBool, AtomicU64, Ordering::Relaxed};
    // (each SYN to a closed port: one RST back, one call of the tap: what the thread took)
    let taken = Arc::new(AtomicU64::new(0));
    let t2 = taken.clone();
    let online = nfsu2_online::OnlineHost::spawn(nfsu2_online::OnlineOptions {
        state_dir: None,
        log: Arc::new(|_: &str, _: Option<&str>| {}),
        tap: Some(Arc::new(move |_: &[u8]| {
            t2.fetch_add(1, Relaxed);
            std::thread::sleep(std::time::Duration::from_micros(100));
        })),
        claims: None,
    });
    let sw = start(LanOptions { max_peers: 2000, max_payload: 1 << 20, ..LanOptions::default() }, Some(online.clone()), None).await;
    let mut floods = Vec::new();
    for i in 0..16 {
        let b = Browser::open(sw.port, &format!("?g=flood{i}"), None).await;
        b.got().drop_frames = true;
        floods.push(b);
    }
    sleep(200).await;
    let (rss0, taken0) = (rss(), taken.load(Relaxed));
    let stop = Arc::new(AtomicBool::new(false));
    let sent = Arc::new(AtomicU64::new(0));
    let mut tasks = Vec::new();
    for b in floods.iter() {
        let syns: Vec<Vec<u8>> = (0..250).map(|i| lan_frame(4, 0, VHOST_VIP, 40000 + i, 1, SYN, i as u32, &[])).collect();
        let bundle = bundle_of(&syns);
        let tx = b.sender();
        let (stop, sent) = (stop.clone(), sent.clone());
        tasks.push(tokio::spawn(async move {
            let mut iv = tokio::time::interval(std::time::Duration::from_millis(100));
            while !stop.load(Relaxed) {
                iv.tick().await;
                tx(bundle.clone());
                sent.fetch_add(250, Relaxed);
            }
        }));
    }
    // every 500 ms, for 3 s: an info() asked now, answered (or not) within 200 ms; nothing here waits for the host
    let t0 = std::time::Instant::now();
    for _ in 0..6 {
        sleep(500).await;
        let answered = tokio::time::timeout(std::time::Duration::from_millis(200), online.info()).await.is_ok();
        let (s, t) = (sent.load(Relaxed), taken.load(Relaxed) - taken0);
        let grown = rss() as i64 - rss0 as i64;
        println!(
            "{:4.1} s: info() {}; {s} frames sent to the host, {t} taken, {} waiting; memory +{} MB ({} bytes a frame waiting)",
            t0.elapsed().as_secs_f64(),
            if answered { "answered in time" } else { "not answered in 200 ms" },
            s.saturating_sub(t),
            grown / (1 << 20),
            grown / s.saturating_sub(t).max(1) as i64
        );
    }
    stop.store(true, Relaxed);
    for t in tasks {
        let _ = t.await;
    }
    // the server stops now (SIGTERM: lan.stop(), which stops the host and waits for its files): the stop waits
    // behind every frame queued before it
    let s = sent.load(Relaxed);
    let t = std::time::Instant::now();
    let lan = sw.lan.clone();
    tokio::task::spawn_blocking(move || lan.stop()).await.unwrap();
    println!("the flood ended ({s} frames sent): the stop took {:.1} s", t.elapsed().as_secs_f64());
    sw.close().await;
}
