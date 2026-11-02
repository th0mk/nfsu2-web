//! A player under a race ban (the crate doc's Kicks and bans) stays on the switch, but no frame that races another
//! player goes from it or to it: LAN play, free roam's outrun and lobby messages (in a bundle too); its other free-roam
//! datagrams pass.  A hello under a name taken away is turned away with `CLOSE_RENAME`.

mod harness;

use bytes::Bytes;
use harness::*;
use nfsu2_lan::CLOSE_RENAME;
use serde_json::json;

const ROAM_PORT: u16 = 47210;

/// a free-roam datagram of type t (runtime/roam_core.h: "N2FR", version 1, type, then the rest of its header)
fn roam(t: u8) -> Vec<u8> {
    let mut d = b"N2FR".to_vec();
    d.extend([1, t]);
    d.extend([0u8; 26]);
    d
}

/// what c got from `from` (sorted: LAN play's datagrams and stream segments wait for clocks of their own): LAN play's
/// kinds, and free roam's datagram types
fn got_from(c: &Client, from: u32) -> Vec<String> {
    let mut v: Vec<String> = c
        .got()
        .dgrams()
        .iter()
        .filter(|d| d.src == from)
        .map(|d| if d.kind == 1 && d.buf.len() > 21 { format!("roam {}", d.buf[21]) } else { format!("kind {}", d.kind) })
        .collect();
    v.sort();
    v
}

/// a sends b LAN play's UDP and TCP, an outrun's, a lobby's and a PING's datagram, and an outrun's in a bundle
fn send_all(a: &Client, b: &Client) {
    a.udp(b.vip, 9999, 9999, b"lan", 3, 0);
    a.udp(b.vip, 50000, 9900, b"seg", 4, 0);
    for t in [9, 12, 6] {
        a.udp(b.vip, ROAM_PORT, ROAM_PORT, &roam(t), 1, 0);
    }
    a.send_bundle(&[(frame_of(b.vip, &roam(9), 1, ROAM_PORT, ROAM_PORT), vec![])]);
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_race_banned_player_races_nobody_and_drives_on() {
    let _s = serial().await;
    let sw = start(opts()).await;
    let mut a = Client::with(sw.port, "Ann", "?norace=pa", Opts { id: Some("pa"), ..lan(1) }).await;
    a.welcome().await;
    let mut b = Client::with(sw.port, "Bob", "?norace=pa", Opts { id: Some("pb"), ..lan(1) }).await;
    b.welcome().await;
    a.until(|g| g.ctl.iter().find(|m| m["t"] == "races").cloned()).await;
    wait_for(2000, || sw.peer_named("Bob").is_some_and(|p| p.hello)).await;
    assert!(b.got().ctl.iter().all(|m| m["t"] != "races"));
    // either way: only the PING
    send_all(&a, &b);
    send_all(&b, &a);
    sleep(300).await;
    assert_eq!(got_from(&b, a.vip), vec!["roam 6"]);
    assert_eq!(got_from(&a, b.vip), vec!["roam 6"]);
    assert!(a.is_open() && b.is_open());

    // the operator lifts it while Ann plays: everything goes; and puts one on Bob
    assert!(sw.lan.set_races(a.vip, false, Bytes::from(json!({ "t": "races", "lifted": true }).to_string())));
    a.until(|g| g.ctl.iter().find(|m| m["lifted"] == true).cloned()).await;
    send_all(&a, &b);
    sleep(300).await;
    assert_eq!(got_from(&b, a.vip), vec!["kind 3", "kind 4", "roam 12", "roam 6", "roam 6", "roam 9", "roam 9"]);
    assert!(sw.lan.set_races(b.vip, true, Bytes::from(json!({ "t": "races", "msg": "x" }).to_string())));
    send_all(&a, &b);
    sleep(300).await;
    assert_eq!(got_from(&b, a.vip).len(), 8, "one more: the PING");
    assert!(!sw.lan.set_races(0x0A4D_1717, true, Bytes::from_static(b"{}")), "nobody there");
    sw.close().await;
}

#[tokio::test(flavor = "multi_thread", worker_threads = 4)]
async fn a_name_taken_away_is_asked_for_another() {
    let _s = serial().await;
    let sw = start(opts()).await;
    let mut c = Client::with(sw.port, "BadName", "?rename=BadName", Opts { id: Some("pc"), ..Opts::default() }).await;
    c.welcome().await;
    assert_eq!(c.closed().await, (CLOSE_RENAME, "rename".into()));
    assert_eq!(c.got().ctl.iter().find(|m| m["t"] == "rename").map(|m| m["name"].clone()), Some(json!("BadName")));
    wait_for(2000, || sw.peers().is_empty()).await;
    assert!(sw.reserved().is_empty(), "its address free at once");
    // under another name: let be
    let mut d = Client::with(sw.port, "GoodName", "?rename=BadName", Opts { id: Some("pc"), ..Opts::default() }).await;
    d.welcome().await;
    sleep(100).await;
    assert!(d.is_open() && sw.has(d.vip));
    sw.close().await;
}
