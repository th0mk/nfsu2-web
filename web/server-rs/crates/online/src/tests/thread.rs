//! The host on its own thread, as the server runs it: frames posted through `Host`, its answers through the
//! `SwitchSend` it was given (with the frame's budgets), info() and vips() after what was posted before them, an
//! overspent client's stream closed, stop() writing the files.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use bytes::Bytes;
use nfsu2_util::{Charge, FrameCtx, Host, SendResult};
use parking_lot::Mutex;

use super::world::ip;
use crate::aries::{FrameReader, encode_frame};
use crate::vhost::{DATA, RST, SYN, SYNACK, lan_frame};
use crate::{OnlineHost, OnlineOptions, PORT_LOBBY, VHOST_VIP};

type Line = (String, Option<String>);

/// a client's budgets: overspent once `over` is set
struct Budget {
    spent: Mutex<usize>,
    over: AtomicBool,
}
impl Charge for Budget {
    fn spend(&self, _frames: u32, bytes: usize) {
        *self.spent.lock() += bytes;
    }
    fn overspent(&self) -> bool {
        self.over.load(Ordering::Relaxed)
    }
}

fn wait_for(what: &str, f: impl Fn() -> bool) {
    let end = Instant::now() + Duration::from_secs(5);
    while !f() {
        assert!(Instant::now() < end, "timed out waiting for {what}");
        std::thread::sleep(Duration::from_millis(5));
    }
}

#[test]
fn on_its_own_thread() {
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("online");
    let lines: Arc<Mutex<Vec<Line>>> = Arc::default();
    let l2 = lines.clone();
    let host = OnlineHost::spawn(OnlineOptions {
        state_dir: Some(dir.clone()),
        log: Arc::new(move |s: &str, k: Option<&str>| l2.lock().push((s.to_string(), k.map(str::to_string)))),
        tap: None,
        claims: None,
    });
    let out: Arc<Mutex<Vec<(u32, Bytes)>>> = Arc::default();
    let o2 = out.clone();
    host.attach_switch(Arc::new(move |vip, f, cause| {
        if let Some(c) = cause {
            c.spend(1, f.len());
        }
        o2.lock().push((vip, f));
        SendResult::Sent
    }));
    let a = ip("10.77.0.2");
    let budget = Arc::new(Budget { spent: Mutex::new(0), over: AtomicBool::new(false) });
    let ctx = |b: &Arc<Budget>| FrameCtx {
        group: "client a".into(),
        site: None,
        cause: Some(b.clone() as Arc<dyn Charge>),
        slot: None,
        owner: None,
    };
    let send = |op: u8, data: &[u8]| Bytes::from(lan_frame(4, a, VHOST_VIP, 50000, PORT_LOBBY, op, 7, data));
    host.frame(a, send(SYN, &[]), ctx(&budget));
    for (kind, f) in
        [("auth", fields![("NAME", "Alpha"), ("PASS", "x")]), ("pers", fields![("PERS", "Alpha")]), ("move", fields![("NAME", "Lounge")])]
    {
        host.frame(a, send(DATA, &encode_frame(kind, "", &f).unwrap()), ctx(&budget));
    }
    let info = host.info().blocking_recv().unwrap();
    assert_eq!((info["sessions"].as_u64(), info["personas"].as_u64(), info["logins"].as_u64()), (Some(1), Some(1), Some(1)));
    assert_eq!(host.vips().blocking_recv().unwrap().into_iter().collect::<Vec<_>>(), [a]);
    assert!(*budget.spent.lock() > 0, "the answers were charged to the client");
    assert!(out.lock().iter().any(|(v, f)| *v == a && f[16] == SYNACK));
    let mut r = FrameReader::new(1 << 16);
    let kinds: Vec<String> = out.lock().iter().filter(|(_, f)| f[16] == DATA).flat_map(|(_, f)| r.push(&f[24..])).map(|m| m.kind).collect();
    assert!(kinds.contains(&"pers".to_string()) && kinds.contains(&"move".to_string()), "{kinds:?}");
    assert!(lines.lock().iter().any(|(s, k)| s.contains("logged in as \"Alpha\"") && k.as_deref() == Some("client a")));
    // the client spent more than its share on a frame: the stream it came on goes, uncharged
    budget.over.store(true, Ordering::Relaxed);
    host.frame(a, send(DATA, &encode_frame("mesg", "", &fields![("TEXT", "hi")]).unwrap()), ctx(&budget));
    wait_for("the stream to go", || out.lock().iter().any(|(_, f)| f[16] == RST));
    let info = host.info().blocking_recv().unwrap();
    assert_eq!((info["overrun"].as_u64(), info["sessions"].as_u64()), (Some(1), Some(0)));
    // a browser that left; the server stops: the files are there when stop() returns
    host.peer_gone(a);
    host.stop();
    assert!(!dir.join("stats.json").exists() && !dir.join("buddies.json").exists(), "nothing to keep: no file");
}

#[test]
fn stop_writes_what_is_kept() {
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("online");
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(dir.join("buddies.json"), r#"{"personas":[{"name":"Alpha","seen":"2025-01","B":["Bravo"]}]}"#).unwrap();
    let host = OnlineHost::spawn(OnlineOptions {
        state_dir: Some(dir.clone()),
        log: Arc::new(|_: &str, _: Option<&str>| {}),
        tap: None,
        claims: None,
    });
    let out: Arc<Mutex<Vec<Bytes>>> = Arc::default();
    let o2 = out.clone();
    host.attach_switch(Arc::new(move |_, f, _| {
        o2.lock().push(f);
        SendResult::Sent
    }));
    let a = ip("10.77.0.2");
    let ctx = FrameCtx { group: "g".into(), site: None, cause: None, slot: None, owner: None };
    let send = |op: u8, port: u16, cid: u32, data: &[u8]| Bytes::from(lan_frame(4, a, VHOST_VIP, 50000 + cid as u16, port, op, cid, data));
    host.frame(a, send(SYN, PORT_LOBBY, 1, &[]), ctx.clone());
    host.frame(a, send(DATA, PORT_LOBBY, 1, &encode_frame("auth", "", &fields![("NAME", "Alpha")]).unwrap()), ctx.clone());
    host.frame(a, send(DATA, PORT_LOBBY, 1, &encode_frame("pers", "", &fields![("PERS", "Alpha")]).unwrap()), ctx.clone());
    let _ = host.info().blocking_recv().unwrap();
    let mut r = FrameReader::new(1 << 16);
    let msgs: Vec<_> = out.lock().iter().filter(|f| f[16] == DATA).flat_map(|f| r.push(&f[24..])).collect();
    let pers = msgs.iter().find(|m| m.kind == "pers").expect("the pers answer");
    let lkey = crate::aries::FieldMap::parse(&pers.body).get("LKEY").unwrap().to_string();
    // the Messenger login: the persona is seen this month, its list is written at the stop
    host.frame(a, send(SYN, crate::PORT_BUDDY, 2, &[]), ctx.clone());
    host.frame(a, send(DATA, crate::PORT_BUDDY, 2, &encode_frame("AUTH", "", &fields![("LKEY", lkey)]).unwrap()), ctx.clone());
    let _ = host.info().blocking_recv().unwrap();
    host.stop();
    let month = crate::jsx::month_of(nfsu2_util::now_ms() as f64);
    let b = std::fs::read_to_string(dir.join("buddies.json")).unwrap();
    assert_eq!(
        b,
        format!(
            r#"{{"v":3,"note":"NFSU2 online Messenger lists (persona names; friend requests and messages waiting for their persona)","personas":[{{"name":"Alpha","seen":"{month}","B":["Bravo"]}}]}}"#
        )
    );
}
