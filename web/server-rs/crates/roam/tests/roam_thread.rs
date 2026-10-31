//! Free roam's hub on its thread, without the switch: frames posted reach the hub, its lists go out through the
//! switch's send, a departure is forgotten, and an idle server keeps its cap on the links (the hub on the switch itself
//! is the server's tests/roam_switch.rs).  A program of its own: on a plain thread the hub's busy measure is the
//! process's CPU time over every core, and the tests beside it in tests/roam_hub.rs (two of them keep cores busy on
//! purpose) filled a 4-core machine's, so that the idle hub cut its links.

mod kit;

use std::collections::HashSet;
use std::sync::Arc;
use std::time::{Duration, Instant};

use bytes::Bytes;
use kit::*;
use nfsu2_roam::{PeerEntry, ROAM_HUB_VIP, RoamHub, RoamOptions};
use nfsu2_util::{FrameCtx, Host, SendResult};
use parking_lot::Mutex;

fn ctx() -> FrameCtx {
    FrameCtx { group: Arc::from("client"), site: None, cause: None, slot: None, owner: None }
}

fn wait_for(what: &str, mut f: impl FnMut() -> bool) {
    let t0 = Instant::now();
    while !f() {
        assert!(t0.elapsed() < Duration::from_secs(5), "{what}: not within 5 s");
        std::thread::sleep(Duration::from_millis(5));
    }
}

#[test]
fn on_its_thread_frames_reach_the_hub_its_lists_go_out_through_the_switch_a_departure_is_forgotten() {
    let lines = Arc::new(Mutex::new(Vec::<String>::new()));
    let l2 = lines.clone();
    let hub = RoamHub::spawn(RoamOptions { tick_ms: 50, ..RoamOptions::default() }, Arc::new(move |s: &str| l2.lock().push(s.into())));
    assert_eq!(hub.vip(), ROAM_HUB_VIP);
    assert_eq!(hub.name(), "free-roam hub");
    assert!(!hub.listed(), "the hub is on nobody's peer list");
    let sent = Arc::new(Mutex::new(Vec::<(u32, Bytes)>::new()));
    let s2 = sent.clone();
    hub.attach_switch(Arc::new(move |vip, f, cause| {
        assert!(cause.is_none(), "the hub's lists are charged to nobody");
        s2.lock().push((vip, f));
        SendResult::Sent
    }));
    let (a, b) = (ip("10.77.0.20"), ip("10.77.0.21"));
    let post = |vip: u32, d: Vec<u8>| hub.frame(vip, Bytes::from(frame(vip, &d)), ctx());
    post(a, hello(501));
    post(a, snap(501, [0.0, 0.0, 0.0]));
    post(b, hello(502));
    post(b, snap(502, [20.0, 0.0, 0.0]));
    // info() answers after what was posted before it
    let info = hub.info().blocking_recv().expect("info");
    assert_eq!(info["players"], 2, "both reached the hub");
    wait_for("Ann's list", || sent.lock().iter().any(|(v, f)| *v == a && f[16 + 16] == 1));
    let got = sent.lock().iter().find(|(v, f)| *v == a && f[16 + 16] == 1).map(|(_, f)| f.clone()).expect("a list");
    assert_eq!(got[0], 1);
    assert_eq!(u32::from_be_bytes(got[4..8].try_into().unwrap()), ROAM_HUB_VIP, "from 10.77.0.253");
    assert_eq!(u32::from_be_bytes(got[8..12].try_into().unwrap()), a);
    assert_eq!(u16::from_be_bytes([got[12], got[13]]), 47210);
    assert_eq!(u16::from_be_bytes([got[14], got[15]]), 47210);
    assert_eq!(peers_of(&got), [PeerEntry { vip: b, port: 47210, id: 502 }]);
    assert_eq!(hub.vips().blocking_recv().expect("vips"), HashSet::from([a, b]));
    hub.peer_gone(b);
    assert_eq!(hub.info().blocking_recv().expect("info")["players"], 1, "Bob left the switch: the hub forgot him");
    let i = hub.info().blocking_recv().expect("info");
    assert!(i["ticks"].as_u64().unwrap_or(0) >= 1 && i["busy"].as_f64().is_some_and(|b| (0.0..=100.0).contains(&b)), "{i}");
    // stop(): no more ticks, and so no more lists; it still answers
    hub.stop();
    let ticks = hub.info().blocking_recv().expect("info")["ticks"].clone();
    let n = sent.lock().len();
    std::thread::sleep(Duration::from_millis(200));
    assert_eq!(sent.lock().len(), n, "no lists after stop()");
    assert_eq!(hub.info().blocking_recv().expect("info")["ticks"], ticks);
    assert!(lines.lock().is_empty(), "an idle server keeps its cap: {:?}", lines.lock());
}
