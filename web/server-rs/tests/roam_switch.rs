//! The switch (nfsu2-lan) with the free-roam hub at 10.77.0.253 (nfsu2-roam) and the Online host beside it, as
//! src/app/run.rs wires them, and browsers on its WebSocket: the hub on the switch.
//!   cargo test --test roam_switch
//! The hub ticks on its own thread every second, as it does in the server (the hub's own tests in crates/roam tick it
//! by hand), so the test waits for the list it looks for.

mod common;

use common::*;
use nfsu2_lan::LanOptions;
use nfsu2_online::VHOST_VIP;
use nfsu2_roam::{ROAM_HUB_VIP, RoamHub, RoamOptions};
use nfsu2_util::Host;
use serde_json::Value;

/// the hub's info() once `f` says yes of it (or after 2 s): the hub takes what the switch posts it on its own thread
async fn hub_until(hub: &RoamHub, f: impl Fn(&Value) -> bool) -> Value {
    let end = std::time::Instant::now() + std::time::Duration::from_secs(2);
    loop {
        let v = hub.info().await.unwrap();
        if f(&v) || std::time::Instant::now() > end {
            return v;
        }
        sleep(20).await;
    }
}

/// the hub's PEERS list to `to` that names someone, out of what that browser got: (frame, first entry's vip, id)
fn list_from_hub(g: &Got, to: u32) -> Option<(Vec<u8>, u32, u32)> {
    g.frames
        .iter()
        .filter(|f| f.len() >= 16 + 17 && f[0] == 1 && be32(f, 4) == ROAM_HUB_VIP && be32(f, 8) == to)
        .find(|f| f[16 + 16] == 1 && f.len() >= 16 + 27)
        .map(|f| (f.clone(), be32(f, 16 + 17), le32(f, 16 + 23)))
}

// on the switch: frames to 10.77.0.253 reach the hub, its lists come back, a departure is forgotten
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn on_the_switch_frames_to_the_hub_reach_it_its_lists_come_back_a_departure_is_forgotten() {
    let hub = roam_hub(RoamOptions::default());
    let (online, _lines) = online_host();
    let sw = start(LanOptions { max_payload: 1 << 20, ..LanOptions::default() }, Some(online), Some(hub.clone())).await;
    let a = Browser::open(sw.port, "", None).await;
    a.hello("Ann");
    let b = Browser::open(sw.port, "", None).await;
    b.hello("Bob");
    a.binary(to_hub(&roam_hello(501)));
    a.binary(to_hub(&roam_snap(501, [0.0, 0.0, 0.0])));
    b.binary(to_hub(&roam_hello(502)));
    b.binary(to_hub(&roam_snap(502, [20.0, 0.0, 0.0])));
    assert_eq!(hub_until(&hub, |v| v["players"] == 2).await["players"], 2, "both reached the hub");
    // its next tick (one a second)
    let (got, vip, id) = a.until_ms(2500, |g| list_from_hub(g, a.vip)).await;
    assert_eq!(be32(&got, 8), a.vip, "Ann got a list from 10.77.0.253");
    assert_eq!(be16(&got, 12), 47210);
    assert_eq!(be16(&got, 14), 47210);
    assert_eq!(vip, b.vip);
    assert_eq!(id, 502);
    let info = sw.info().await;
    let peers = info["peers"].as_array().unwrap();
    assert_eq!(peers.len(), 2);
    assert!(!peers.iter().any(|p| p["vip"] == "10.77.0.253"), "the hub is on nobody's peer list");
    // the browsers' own lists: the two of them and the Online host, never the hub
    let list = a.until(|g| g.peers().filter(|l| l.len() == 3)).await;
    assert!(list.iter().all(|p| p["vip"] != ROAM_HUB_VIP), "{list:?}");
    assert!(list.iter().any(|p| p["vip"] == VHOST_VIP && p["server"] == true));
    let n = sw.lan.breakdown().await;
    assert_eq!((n.players, n.roam), (2, 2), "both counted in free roam (the console's [players] line): {n:?}");
    assert!(wait_for(2500, || hub.in_roam() == 2).await, "both driving in free roam (/players): {}", hub.in_roam());
    b.close().await;
    assert_eq!(hub_until(&hub, |v| v["players"] == 1).await["players"], 1, "Bob left the switch: the hub forgot him");
    assert!(wait_for(2500, || hub.in_roam() == 1).await, "and at its next tick /players: {}", hub.in_roam());
    let roam: Value = sw.info().await["roam"].clone();
    assert_eq!(roam["players"], 1, "{roam}");
    a.close().await;
    sw.close().await;
}

// a browser whose connection just ends, without a close frame, is forgotten by the hub as well
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_connection_that_ends_without_a_close_frame_is_forgotten_by_the_hub() {
    let hub = roam_hub(RoamOptions::default());
    let sw = start(LanOptions::default(), None, Some(hub.clone())).await;
    let a = Browser::open(sw.port, "", None).await;
    a.hello("Ann");
    a.binary(to_hub(&roam_hello(601)));
    a.binary(to_hub(&roam_snap(601, [0.0, 0.0, 0.0])));
    a.flush().await;
    assert_eq!(hub_until(&hub, |v| v["players"] == 1).await["players"], 1);
    assert_eq!(hub.vips().await.unwrap().len(), 1);
    a.terminate();
    assert!(wait_for(2000, || sw.lan.peer_count() == 0).await, "Ann is off the switch");
    assert_eq!(hub_until(&hub, |v| v["players"] == 0).await["players"], 0, "the hub forgot her");
    assert!(hub.vips().await.unwrap().is_empty());
    sw.close().await;
}

/// a bundle (the switch's frame kind 32) of (frame, destinations) entries: none for the frame's own destination
fn bundle_to(entries: &[(Vec<u8>, Vec<u32>)]) -> Vec<u8> {
    let mut b = vec![32, 0, 0, 0];
    for (f, to) in entries {
        b.push(to.len() as u8);
        b.push(0);
        b.extend_from_slice(&(f.len() as u16).to_be_bytes());
        for v in to {
            b.extend_from_slice(&v.to_be_bytes());
        }
        b.extend_from_slice(f);
    }
    b
}

// bundles with the hub on the switch: a page that takes bundles sends its datagrams in them, an entry to the hub by
// its own destination, and one whose list names the hub and a friend: the hub takes both (the friend's SNAP links
// the two), the friend gets its copy with the sender's address
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn datagrams_to_the_hub_inside_bundles_reach_it() {
    let hub = roam_hub(RoamOptions::default());
    let sw = start(LanOptions::default(), None, Some(hub.clone())).await;
    let a = Browser::open(sw.port, "", None).await;
    a.text(r#"{"t":"hello","name":"Ann","bundle":1}"#);
    let b = Browser::open(sw.port, "", None).await;
    b.text(r#"{"t":"hello","name":"Bob","bundle":1}"#);
    sleep(50).await;
    let snap = udp(b.vip, 47210, 47210, 1, &roam_snap(701, [0.0, 0.0, 0.0]));
    a.binary(bundle_to(&[(to_hub(&roam_hello(701)), vec![]), (snap, vec![ROAM_HUB_VIP, b.vip])]));
    b.binary(to_hub(&roam_hello(702)));
    b.binary(to_hub(&roam_snap(702, [10.0, 0.0, 0.0])));
    let got = b.until(|g| g.frames.iter().find(|f| f[0] == 1 && be32(f, 4) == a.vip).cloned()).await;
    assert_eq!(be32(&got, 8), b.vip, "the friend's copy, to its own address");
    assert_eq!(hub_until(&hub, |v| v["players"] == 2).await["players"], 2, "both reached the hub");
    // Ann's SNAP came to the hub only in the bundle's list: the hub's next tick links the two
    let (_, vip, id) = a.until_ms(2500, |g| list_from_hub(g, a.vip)).await;
    assert_eq!((vip, id), (b.vip, 702));
    let info = sw.info().await;
    assert_eq!(info["limited"], 0, "{info}");
    a.close().await;
    b.close().await;
    sw.close().await;
}

// a claimed name (nfsu2-claims) on the switch: the hello's "claim" names the keyphrase its frames come with, and the
// hub links a player under a claimed name only when that keyphrase claimed it
#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_claimed_name_is_its_keyphrases_on_the_switch() {
    let claims = std::sync::Arc::new(nfsu2_claims::Claims::in_memory());
    let (a_token, c_token) = ("A".repeat(43), "C".repeat(43));
    claims.claim("Alex", &nfsu2_claims::owner_of_token(&a_token).unwrap(), nfsu2_util::now_ms()).unwrap();
    let lines = std::sync::Arc::new(parking_lot::Mutex::new(Vec::<String>::new()));
    let l2 = lines.clone();
    let hub = RoamHub::spawn_with(RoamOptions::default(), std::sync::Arc::new(move |l: &str| l2.lock().push(l.to_string())), Some(claims));
    let sw = start(LanOptions { max_payload: 1 << 20, ..LanOptions::default() }, None, Some(hub.clone())).await;
    let hello = |b: &Browser, name: &str, claim: Option<&str>| {
        let mut m = serde_json::json!({ "t": "hello", "name": name });
        if let Some(c) = claim {
            m["claim"] = c.into();
        }
        b.text(&m.to_string());
    };
    // the owner, a player of no keyphrase and one of another under the same name, and a bystander
    let a = Browser::open(sw.port, "", None).await;
    hello(&a, "Alex", Some(&a_token));
    let b = Browser::open(sw.port, "", None).await;
    hello(&b, "Bob", None);
    let c = Browser::open(sw.port, "", None).await;
    hello(&c, "Alex", Some(&c_token));
    let d = Browser::open(sw.port, "", None).await;
    hello(&d, "Alex", None);
    for (x, id, name, at) in [(&a, 501, &b"Alex"[..], 0.0), (&b, 502, b"Bob", 10.0), (&c, 503, b"alex", 20.0), (&d, 504, b"Alex", 30.0)] {
        x.binary(to_hub(&roam_hello_named(id, name)));
        x.binary(to_hub(&roam_snap(id, [at, 0.0, 0.0])));
    }
    assert_eq!(hub_until(&hub, |v| v["players"] == 4).await["players"], 4);
    let (_, vip, _) = b.until_ms(2500, |g| list_from_hub(g, b.vip)).await;
    assert_eq!(vip, a.vip, "Bob's list: the owner, nearest of the two it may see");
    sleep(1200).await;
    let named = |x: &Browser| {
        b.got().frames.iter().any(|f| {
            f.len() >= 16 + 27 && f[0] == 1 && be32(f, 8) == b.vip && f[16 + 16] >= 1 && {
                let n = f[16 + 16] as usize;
                (0..n).any(|i| be32(f, 16 + 17 + 10 * i) == x.vip)
            }
        })
    };
    assert!(named(&a), "the owner is linked");
    assert!(!named(&c) && !named(&d), "the others under its name are not");
    let l = lines.lock().clone();
    assert_eq!(l.iter().filter(|l| l.contains("a name another keyphrase claimed")).count(), 2, "{l:?}");
    for x in [a, b, c, d] {
        x.close().await;
    }
    sw.close().await;
}
