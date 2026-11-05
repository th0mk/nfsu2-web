mod kit;
use kit::*;
use nfsu2_roam::RoamOptions;
const A: u32 = 0x0a4d0002;
fn players(k: &mut Kit, n: u32, room: u32, x: f64) {
    for i in 0..n {
        k.from(A + i, &hello_with(100 + i, &HelloOpts { room, ..Default::default() }));
        k.from(A + i, &snap_moving(100 + i, [x + (i % 4) as f64, 100.0, 12.0], [0.0; 3], 0.0, 2));
    }
}
fn query(k: &mut Kit, vip: u32, room: u32) -> Option<Vec<u8>> {
    k.clear();
    let mut b = b"MTQ1".to_vec();
    b.extend(room.to_le_bytes());
    b.extend(1u32.to_le_bytes());
    k.from(vip, &b);
    k.sent.lock().iter().find(|(_, f)| f.get(16..20) == Some(b"MTP1")).map(|(_, f)| f[16..].to_vec())
}
#[test]
fn detects_without_requests_then_serves_cached_room_state() {
    let mut k = Kit::new(RoamOptions::default());
    assert!(query(&mut k, A, 0).is_none());
    players(&mut k, 20, 0, 100.0);
    k.hub.tick();
    assert_eq!(query(&mut k, A, 0).unwrap()[12], 0);
    for _ in 0..30 {
        k.step(2000.0);
        players(&mut k, 20, 0, 100.0);
        k.hub.tick();
    }
    let m = query(&mut k, A, 0).unwrap();
    assert_eq!(m[12], 1);
    assert_eq!(u16::from_le_bytes([m[19], m[20]]), 20);
    assert!(query(&mut k, A, 0).is_none(), "rate limited per browser");
    k.step(10_000.0);
    assert!(query(&mut k, A, 99).is_none(), "cannot request another room");
    // Queries do not recompute: the next timer update removes a departed crowd.
    for i in 1..20 {
        k.hub.peer_gone(A + i);
    }
    assert_eq!(query(&mut k, A, 0).unwrap()[12], 1);
    k.from(A, &hello_with(100, &HelloOpts::default()));
    k.hub.tick();
    k.step(10_000.0);
    assert_eq!(query(&mut k, A, 0).unwrap()[12], 0);
}
#[test]
fn separates_rooms_and_expires_stale_positions() {
    let mut k = Kit::new(RoamOptions::default());
    for _ in 0..32 {
        players(&mut k, 19, 0, 100.0);
        k.from(A + 19, &hello_with(119, &HelloOpts { room: 42, ..Default::default() }));
        k.from(A + 19, &snap_moving(119, [100.0, 100.0, 12.0], [0.0; 3], 0.0, 2));
        k.hub.tick();
        k.step(2000.0);
    }
    assert_eq!(query(&mut k, A, 0).unwrap()[12], 0);
    assert_eq!(query(&mut k, A + 19, 42).unwrap()[12], 0);
    k.step(10_000.0);
    players(&mut k, 20, 0, 100.0);
    k.hub.tick();
    assert_eq!(query(&mut k, A, 0).unwrap()[12], 1);
    k.step(10_000.0);
    k.from(A, &hello_with(100, &HelloOpts::default()));
    k.hub.tick();
    assert_eq!(query(&mut k, A, 0).unwrap()[12], 0);
}

#[test]
fn native_client_polls_and_reads_real_server_cache_without_map() {
    use std::{path::Path, process::Command};
    let repo = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../..");
    let exe = Path::new(env!("CARGO_TARGET_TMPDIR")).join("car_meets_wire");
    let cc = Command::new("cc")
        .current_dir(&repo)
        .args(["-O2", "-Iruntime", "web/test/roam_hubwire.c", "runtime/roam_core.c", "-o"])
        .arg(&exe)
        .output()
        .unwrap();
    assert!(cc.status.success(), "{}", String::from_utf8_lossy(&cc.stderr));
    let run = |args: &[String]| {
        let out = Command::new(&exe).args(args).output().unwrap();
        assert!(out.status.success());
        String::from_utf8(out.stdout).unwrap()
    };
    let ask = run(&["meets".into()]);
    let hex = ask.lines().next().unwrap().strip_prefix("ask ").unwrap();
    let ask: Vec<u8> = (0..hex.len()).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap()).collect();
    let mut k = Kit::new(RoamOptions::default());
    for _ in 0..32 {
        players(&mut k, 20, 0, 100.0);
        k.hub.tick();
        k.step(2000.0);
    }
    k.clear();
    k.from(A, &ask);
    let reply = k.sent.lock().iter().find(|(_, f)| f.get(16..20) == Some(b"MTP3")).unwrap().1[16..].to_vec();
    let hex = reply.iter().map(|x| format!("{x:02x}")).collect::<String>();
    let got = run(&["meets".into(), hex]);
    assert!(got.lines().any(|l| l == "meets 1 102,100,12,20"));
    let id = u32::from_le_bytes(reply[21..25].try_into().unwrap());
    assert_eq!(reply[25], 24);
    for (j, npc) in reply[26..].chunks_exact(15).enumerate() {
        let p: Vec<_> = npc[..12].chunks_exact(4).map(|v| i32::from_le_bytes(v.try_into().unwrap())).collect();
        let heading = u16::from_le_bytes(npc[12..14].try_into().unwrap());
        let expected = format!("npc {id} {j} {},{},{} {heading} {}", p[0], p[1], p[2], npc[14]);
        assert!(got.lines().any(|l| l == expected), "native client must preserve every server placement: {expected}");
    }
}

#[test]
fn excludes_spawn_and_moving_cars() {
    let mut k = Kit::new(RoamOptions::default());
    for _ in 0..32 {
        for i in 0..40 {
            k.from(A + i, &hello_with(100 + i, &HelloOpts::default()));
            let (p, v) = if i < 20 { ([1695.3, -883.6, 24.0], [0.0; 3]) } else { ([100.0, 100.0, 12.0], [4.0, 0.0, 0.0]) };
            k.from(A + i, &snap_moving(100 + i, p, v, 0.0, 2));
        }
        k.hub.tick();
        k.step(2000.0);
    }
    assert_eq!(query(&mut k, A, 0).unwrap()[12], 0);
}

#[test]
fn clients_receive_identical_stable_server_crowds() {
    let mut k = Kit::new(RoamOptions::default());
    for _ in 0..32 {
        players(&mut k, 20, 0, 100.0);
        k.hub.tick();
        k.step(2000.0);
    }
    let ask = |k: &mut Kit, who| {
        k.clear();
        let mut request = b"MTQ3".to_vec();
        request.extend(0u32.to_le_bytes());
        request.extend(1u32.to_le_bytes());
        k.from(who, &request);
        k.sent.lock().iter().find(|(_, f)| f.get(16..20) == Some(b"MTP3")).unwrap().1[16..].to_vec()
    };
    let first = ask(&mut k, A);
    assert_eq!(first, ask(&mut k, A + 1), "same room, same layout, regardless of requester");
    assert_eq!(first[12], 1);
    assert_eq!(first[25], 24);
    // A changing centroid/count must not move the NPCs or change their identity.
    k.step(10_000.0);
    players(&mut k, 21, 0, 103.0);
    k.hub.tick();
    let changed = ask(&mut k, A);
    assert_eq!(&first[21..], &changed[21..], "stable ID, positions, headings and variants");
    for i in 1..21 {
        k.hub.peer_gone(A + i);
    }
    k.step(10_000.0);
    k.from(A, &hello_with(100, &HelloOpts::default()));
    k.hub.tick();
    assert_eq!(ask(&mut k, A)[12], 0, "crowd disappears with the meet");
}

#[test]
fn full_catalog_fits_datagram_and_legacy_clients_keep_small_crowds() {
    let mut k = Kit::new(RoamOptions::default());
    for _ in 0..32 {
        for i in 0..160u32 {
            k.from(A + i, &hello_with(100 + i, &HelloOpts::default()));
            k.from(A + i, &snap_moving(100 + i, [100.0 + (i / 20) as f64 * 300.0, 100.0, 12.0], [0.0; 3], 0.0, 2));
        }
        k.hub.tick();
        k.step(2000.0);
    }
    for (version, expected_counts) in [(b'3', vec![24, 24, 0, 0, 0, 0, 0, 0]), (b'2', vec![8; 8])] {
        k.clear();
        let mut request = vec![b'M', b'T', b'Q', version];
        request.extend(0u32.to_le_bytes());
        request.extend(1u32.to_le_bytes());
        k.from(A, &request);
        let sent = k.sent.lock();
        let reply = &sent.iter().find(|(_, f)| f.get(16..19) == Some(b"MTP")).unwrap().1[16..];
        assert_eq!(reply.len(), if version == b'3' { 837 } else { 1077 });
        assert_eq!(reply[12], 8);
        let mut at = 13;
        let mut counts = Vec::new();
        let mut detailed_x = Vec::new();
        for _ in 0..8 {
            let n = reply[at + 12] as usize;
            if n == 24 {
                detailed_x.push(i16::from_le_bytes([reply[at], reply[at + 1]]));
            }
            counts.push(n);
            at += 13 + n * 15;
        }
        assert_eq!(at, reply.len());
        counts.sort_by(|a, b| b.cmp(a));
        assert_eq!(counts, expected_counts);
        if version == b'3' {
            detailed_x.sort();
            assert_eq!(detailed_x, vec![100, 400]);
        }
        drop(sent);
        k.step(10_000.0);
    }
}

#[test]
fn stacked_roads_have_separate_cached_meets() {
    let mut k = Kit::new(RoamOptions::default());
    for _ in 0..32 {
        for i in 0..40u32 {
            k.from(A + i, &hello_with(100 + i, &HelloOpts::default()));
            let z = if i < 20 { 12.0 } else { 32.0 };
            k.from(A + i, &snap_moving(100 + i, [100.0, 100.0, z], [0.0; 3], 0.0, 2));
        }
        k.hub.tick();
        k.step(2000.0);
    }
    let reply = query(&mut k, A, 0).unwrap();
    assert_eq!(reply[12], 2);
    let mut heights = Vec::new();
    for m in reply[13..].chunks_exact(8) {
        heights.push(i16::from_le_bytes([m[4], m[5]]));
        assert_eq!(u16::from_le_bytes([m[6], m[7]]), 20);
    }
    heights.sort();
    assert_eq!(heights, vec![12, 32]);
}
