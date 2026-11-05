mod kit;
use kit::*;
use nfsu2_roam::RoamOptions;
use serde_json::Value;
const A: u32 = 0x0a4d0002;
const B: u32 = 0x0a4d0003;
fn test_kit() -> Kit {
    // This integration-test binary owns one fixed catalog, shared by all its parallel tests.
    static INIT: std::sync::Once = std::sync::Once::new();
    INIT.call_once(|| nfsu2_roam::speedtrap::set_dev_catalog(nfsu2_roam::speedtrap::parse_catalog(
        "TRAP(airport,1711,-773,24.6,1743,-779,24.6)"
    ).unwrap()));
    Kit::new(RoamOptions::default())
}
fn join(k: &mut Kit, vip: u32, id: u32, room: u32, name: &[u8]) {
    k.from(vip, &hello_with(id, &HelloOpts { room, name, ..Default::default() }));
}
fn pose(k: &mut Kit, vip: u32, id: u32, y: f64, tick: u32) {
    let mut d = snap_moving(id, [1727., y, 24.6], [0., 60., 0.], std::f64::consts::FRAC_PI_2, 2);
    d[22..26].copy_from_slice(&tick.to_le_bytes());
    let n = d.len();
    let crc = crc32fast::hash(&d[..n - 4]);
    d[n - 4..].copy_from_slice(&crc.to_le_bytes());
    k.from(vip, &d);
}
fn board(k: &mut Kit, vip: u32) -> Option<Value> {
    k.clear();
    k.from(vip, b"STQ1");
    let sent = k.sent.lock();
    sent.iter().find(|(_, f)| f.get(16..20) == Some(b"STP1".as_slice())).map(|(_, f)| serde_json::from_slice(&f[20..]).unwrap())
}
#[test]
fn scores_crossings_isolates_rooms_and_rate_limits_replies() {
    let mut k = test_kit();
    assert!(board(&mut k, A).is_none(), "unknown browser gets no board");
    join(&mut k, A, 10, 0, b"Alice");
    join(&mut k, B, 20, 42, b"Bob");
    pose(&mut k, A, 10, -806., 1_000_000);
    k.step(1000.);
    pose(&mut k, A, 10, -746., 2_000_000);
    let a = board(&mut k, A).unwrap();
    assert_eq!(a["best"], 2160);
    assert_eq!(a["passes"], 1);
    assert_eq!(a["rows"][0]["name"], "Alice");
    assert_eq!(a["rows"][0]["me"], true);
    assert!(board(&mut k, A).is_none(), "one reply a second per browser");
    let b = board(&mut k, B).unwrap();
    assert_eq!(b["rows"].as_array().unwrap().len(), 0);
    k.step(1000.);
    join(&mut k, B, 20, 0, b"Bob");
    assert_eq!(board(&mut k, B).unwrap()["rows"][0]["name"], "Alice");
    k.step(1000.);
    join(&mut k, A, 10, 42, b"Alice");
    assert_eq!(board(&mut k, A).unwrap()["best"], 0, "room changes clear scores and samples");
}
#[test]
fn wrong_sender_cannot_score_and_departures_remove_rows() {
    let mut k = test_kit();
    join(&mut k, A, 10, 0, b"Alice");
    join(&mut k, B, 20, 0, b"Bob");
    pose(&mut k, A, 10, -806., 1_000_000);
    k.step(1000.);
    pose(&mut k, A, 999, -746., 2_000_000);
    assert_eq!(board(&mut k, A).unwrap()["best"], 0);
    pose(&mut k, A, 10, -746., 2_000_000);
    k.step(1000.);
    assert_eq!(board(&mut k, B).unwrap()["rows"].as_array().unwrap().len(), 1);
    k.hub.peer_gone(A);
    k.step(1000.);
    assert!(board(&mut k, B).unwrap()["rows"].as_array().unwrap().is_empty());
}
#[test]
fn blocked_players_are_absent_and_new_sessions_start_empty() {
    let mut k = test_kit();
    join(&mut k, A, 10, 0, b"Alice");
    join(&mut k, B, 20, 0, b"Bob");
    pose(&mut k, A, 10, -806., 1_000_000);
    k.step(1000.);
    pose(&mut k, A, 10, -746., 2_000_000);
    k.hub.blocks(B, vec!["Alice".into()]);
    assert!(board(&mut k, B).unwrap()["rows"].as_array().unwrap().is_empty());
    join(&mut k, A, 11, 0, b"Alice");
    assert_eq!(board(&mut k, A).unwrap()["best"], 0);
}

#[test]
fn crossing_pushes_once_to_a_subscribed_endpoint_without_waiting_for_polling() {
    let mut k = test_kit();
    join(&mut k, A, 10, 0, b"Alice");
    pose(&mut k, A, 10, -806., 1_000_000);
    k.step(500.);
    assert_eq!(board(&mut k, A).unwrap()["sequence"], 0);
    k.clear();
    k.step(500.);
    pose(&mut k, A, 10, -746., 2_000_000);
    let messages: Vec<_> = k
        .sent
        .lock()
        .iter()
        .filter(|(_, f)| f.get(16..20) == Some(b"STP1".as_slice()))
        .map(|(_, f)| {
            assert!(f.len() <= 16 + 1472);
            serde_json::from_slice::<Value>(&f[20..]).unwrap()
        })
        .collect();
    assert_eq!(messages.len(), 1);
    assert_eq!(messages[0]["sequence"], 1);
    assert_eq!(messages[0]["event"]["id"], "airport");
    assert_eq!(messages[0]["event"]["speed"], 2160);
    assert!(board(&mut k, A).is_none(), "push does not relax the request limiter");
    k.step(1000.);
    let state = board(&mut k, A).unwrap();
    assert_eq!(state["sequence"], 1, "polling preserves the crossing id");
    assert_eq!(state["nearby"][0]["id"], "airport");
}

#[test]
fn banner_board_is_ordered_by_best_speed_for_the_crossed_trap() {
    let mut k = test_kit();
    for i in 0..5u32 {
        join(&mut k, A+i, 10+i, 0, &[b'A'+i as u8]);
        let mut data=snap_moving(10+i,[1727.,-806.,24.6],[0.,50.+i as f64*10.,0.],std::f64::consts::FRAC_PI_2,2);
        data[22..26].copy_from_slice(&1_000_000u32.to_le_bytes());
        let n=data.len();let crc=crc32fast::hash(&data[..n-4]);data[n-4..].copy_from_slice(&crc.to_le_bytes());k.from(A+i,&data);
    }
    k.step(1000.);
    for i in 0..5u32 {
        let mut data=snap_moving(10+i,[1727.,-746.,24.6],[0.,50.+i as f64*10.,0.],std::f64::consts::FRAC_PI_2,2);
        data[22..26].copy_from_slice(&2_000_000u32.to_le_bytes());
        let n=data.len();let crc=crc32fast::hash(&data[..n-4]);data[n-4..].copy_from_slice(&crc.to_le_bytes());k.from(A+i,&data);
    }
    let state=board(&mut k,A).unwrap();
    let rows=state["rows"].as_array().unwrap();
    assert_eq!(rows.iter().take(3).map(|r|r["name"].as_str().unwrap()).collect::<Vec<_>>(),vec!["E","D","C"]);
    assert_eq!(rows[0]["speed"],3240);
    assert_eq!(rows[4]["me"],true);
}
