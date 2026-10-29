//! A rough measure of the host's own cost (ignored by default; `cargo test -p nfsu2-online --release load --
//! --ignored --nocapture`): 900 lobby sessions in rooms of 50 chatting, with EA Messenger and presence.

use std::time::Instant;

use super::world::{C, Opts, World};

#[test]
#[ignore]
fn lobby_load() {
    let mut w = World::new(Opts::default());
    let t = Instant::now();
    let mut lobby: Vec<C> = Vec::new();
    let mut buddies: Vec<C> = Vec::new();
    for i in 0..900u32 {
        let g = w.client_of(&format!("10.77.{}.{}", 1 + i / 200, 2 + i % 200), &format!("client {i}"));
        let c = w.login(g, &format!("Player{i}"));
        let k = w.lkey(c);
        buddies.push(w.buddy(g, &k));
        lobby.push(c);
    }
    let login = t.elapsed();
    let rooms = ["Lounge", "Circuit", "Sprint", "Drift", "Drag", "StreetX", "URL"];
    for (i, c) in lobby.iter().enumerate().take(7 * 50) {
        w.send(*c, "move", fields![("NAME", rooms[i % 7])]);
    }
    let frames0 = w.core.info()["framesOut"].as_u64().unwrap();
    w.mute = true;
    let t = Instant::now();
    let mut msgs = 0;
    for round in 0..20 {
        for (i, c) in lobby.iter().enumerate().take(7 * 50) {
            if i % 10 == round % 10 {
                w.send(*c, "mesg", fields![("TEXT", format!("hello {round}"))]);
                msgs += 1;
            }
        }
        for (i, b) in buddies.iter().enumerate().take(300) {
            w.send(*b, "PSET", fields![("SHOW", "CHAT"), ("PROD", format!("p{}", (round + i) % 3))]);
        }
        w.advance(1100.0);
    }
    let el = t.elapsed();
    let frames = w.core.info()["framesOut"].as_u64().unwrap() - frames0;
    eprintln!(
        "900 logins with Messenger: {login:?}; {msgs} chat lines to rooms of 50 and 6000 presences: {el:?}, {frames} frames out ({:.0} frames/s)",
        frames as f64 / el.as_secs_f64()
    );
}

/// What one frame of a client costs the host where the lobby is full of others (1000 sessions): a lobby connection
/// refused (the sessions of its browser, client and site counted) and a Messenger login with a key of nobody's
/// (looked for among the sessions, then tried again 6 times, 500 ms apart).
#[test]
#[ignore]
fn costly_frames() {
    use crate::vhost::{RST, SYN, lan_frame};
    use crate::{PORT_BUDDY, PORT_LOBBY, VHOST_VIP};

    let mut w = World::new(Opts::default());
    for i in 0..1000u32 {
        let g = w.client_of(&format!("10.77.{}.{}", 1 + i / 200, 2 + i % 200), &format!("client {i}"));
        w.login(g, &format!("Player{i}"));
    }
    w.mute = true;
    let g = w.client_of("10.77.9.2", "client 0");
    // a browser of a client that holds its share: each lobby connection refused
    let n = 20_000u32;
    let t = Instant::now();
    for k in 0..n {
        w.raw(g, &lan_frame(4, g, VHOST_VIP, 1000 + (k % 60000) as u16, PORT_LOBBY, SYN, k, &[]));
    }
    let syn = t.elapsed();
    // a Messenger login with a key of nobody's, and the time for its tries again
    let t = Instant::now();
    for k in 0..n {
        let port = 1000 + (k % 60000) as u16;
        w.raw(g, &lan_frame(4, g, VHOST_VIP, port, PORT_BUDDY, SYN, k, &[]));
        let auth = crate::aries::encode_frame("AUTH", "", &fields![("LKEY", "0123456789abcdef01234567")]).unwrap();
        w.raw(g, &lan_frame(4, g, VHOST_VIP, port, PORT_BUDDY, crate::vhost::DATA, k, &auth));
        w.raw(g, &lan_frame(4, g, VHOST_VIP, port, PORT_BUDDY, RST, k, &[]));
        if k % 1000 == 999 {
            w.sleep(3500.0);
        }
    }
    w.sleep(3500.0);
    let auth = t.elapsed();
    eprintln!(
        "with 1000 sessions: a refused lobby connection {:.2} us, a Messenger login with nobody's key (and its 6 tries again) {:.2} us",
        syn.as_secs_f64() * 1e6 / n as f64,
        auth.as_secs_f64() * 1e6 / n as f64
    );
}

/// The results' save, on the host's thread as in Node (the lobby waits for it, at most every 30 s): a full store,
/// 100,000 personas with races of two types and five best times each.
#[test]
#[ignore]
fn stats_save() {
    use std::cell::Cell;
    use std::rc::Rc;

    use crate::stats::{Race, RacePlayer, Stats, StatsOptions};

    let dir = tempfile::tempdir().unwrap();
    let t = Rc::new(Cell::new(1_792_411_200_000.0));
    let mut o = StatsOptions::new(Some(dir.path().to_path_buf()), crate::stats::tests::clocks(t), Rc::new(|_: &str, _: Option<&str>| {}));
    o.save_ms = 1e12;
    let mut st = Stats::new(o);
    for i in 0..50_000u32 {
        for track in 0..5 {
            let players = vec![
                RacePlayer { name: format!("Player{}", 2 * i), place: 1, time: 60.0 + track as f64 },
                RacePlayer { name: format!("Player{}", 2 * i + 1), place: 2, time: 61.5 + track as f64 },
            ];
            st.add_race(&Race { kind: track % 2, ranked: true, track: 4000 + track as i64, players });
        }
    }
    let t0 = Instant::now();
    st.stop();
    let el = t0.elapsed();
    let size = std::fs::metadata(dir.path().join("stats.json")).unwrap().len();
    eprintln!("stats.json of 100,000 personas: {} MB written in {el:?}", size / 1_000_000);
}

/// How many frames a second the host's own thread takes from the switch (the queue between them has no bound of its
/// own: what bounds it is the switch's budgets, charged before a frame is posted, 10,000 frames a second for one
/// site): four threads post the cheapest frames there are (a UDP datagram, answered "port unreachable"; a stream
/// segment for no stream, answered RST; an echo request), the host answers through a switch that only counts.
#[test]
#[ignore]
fn frame_flood() {
    use std::sync::Arc;
    use std::sync::atomic::{AtomicUsize, Ordering};

    use bytes::Bytes;
    use nfsu2_util::{FrameCtx, Host, SendResult};

    use crate::vhost::{DATA, lan_frame};
    use crate::{OnlineHost, OnlineOptions, VHOST_VIP};

    let host = OnlineHost::spawn(OnlineOptions { state_dir: None, log: Arc::new(|_: &str, _: Option<&str>| {}), tap: None, claims: None });
    let out = Arc::new(AtomicUsize::new(0));
    let o2 = out.clone();
    host.attach_switch(Arc::new(move |_, _, _| {
        o2.fetch_add(1, Ordering::Relaxed);
        SendResult::Sent
    }));
    let per_thread = 250_000;
    for (what, kind, op) in [("UDP", 3u8, 0u8), ("TCP for no stream", 4, DATA), ("ICMP echo", 5, 0)] {
        let before = out.load(Ordering::Relaxed);
        let t = Instant::now();
        std::thread::scope(|sc| {
            for k in 0..4u32 {
                let host = host.clone();
                sc.spawn(move || {
                    let vip = 0x0A4D_0100 + k;
                    let payload: &[u8] = if kind == 5 { &[8, 0, 0, 0, 0, 1, 0, 1] } else { b"x" };
                    let f = Bytes::from(lan_frame(kind, vip, VHOST_VIP, 3658, 3658, op, 1, payload));
                    let ctx = FrameCtx { group: format!("client {k}").into(), site: None, cause: None, slot: None, owner: None };
                    for _ in 0..per_thread {
                        host.frame(vip, f.clone(), ctx.clone());
                    }
                });
            }
        });
        let posted = t.elapsed();
        let _ = host.info().blocking_recv();
        let el = t.elapsed();
        let n = 4 * per_thread;
        eprintln!(
            "{what}: {n} frames posted in {posted:?}, taken in {el:?} ({:.0} frames/s), {} answers",
            n as f64 / el.as_secs_f64(),
            out.load(Ordering::Relaxed) - before
        );
    }
    host.stop();
}
