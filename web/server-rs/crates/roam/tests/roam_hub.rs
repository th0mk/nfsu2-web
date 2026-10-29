//! Free roam's hub: which players it links, what it reads and sends.
//! 1. the wire: what runtime/roam_core.c sends the hub (HELLO, SNAP, BYE, encoded by the C core built for wasm,
//!    web/test/roam_hubwire.c) is what the hub reads, and the hub's PEERS lists decode in the C core;
//! 2. the links: nearest first, both ends of a link list each other, at most 32 per player (7 for a core of version
//!    1), rooms apart, stale positions out, links kept at the edge, BYE and departures forgotten, the limits per
//!    address;
//! 3. the hub on its thread, without the switch: frames posted reach the hub, its lists go out through the switch's
//!    send, a departure is forgotten (the hub on the switch itself is the server's tests/roam_switch.rs).

mod kit;

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use bytes::Bytes;
use kit::*;
use nfsu2_roam::{Hub, PeerEntry, ROAM_HUB_VIP, RoamHub, RoamMsg, RoamOptions, decode_roam, encode_peers};
use nfsu2_util::{FrameCtx, Host, SendResult};
use parking_lot::Mutex;

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../..")
}

fn have_node() -> bool {
    Command::new("node").arg("--version").output().is_ok_and(|o| o.status.success())
}

// the hub's address is net_proto.h's and proto.ts's NET_ROAM_HUB
#[test]
fn the_hubs_address_is_net_proto_h_and_proto_ts_net_roam_hub() {
    let h = std::fs::read_to_string(repo().join("runtime/web/net_proto.h")).expect("net_proto.h");
    let ts = std::fs::read_to_string(repo().join("web/src/net/proto.ts")).expect("proto.ts");
    // /#define NET_ROAM_HUB\s+0x([0-9A-Fa-f]+)u/ and /NET_ROAM_HUB = 0x([0-9a-f]+)/
    let hex_after = |s: &str, pat: &str| -> u32 {
        let at = s.find(pat).unwrap_or_else(|| panic!("{pat}")) + pat.len();
        let rest = s[at..].trim_start();
        let digits: String = rest.strip_prefix("0x").expect("0x").chars().take_while(|c| c.is_ascii_hexdigit()).collect();
        u32::from_str_radix(&digits, 16).expect("hex")
    };
    assert_eq!(hex_after(&h, "#define NET_ROAM_HUB"), ROAM_HUB_VIP);
    assert_eq!(hex_after(&ts, "NET_ROAM_HUB ="), ROAM_HUB_VIP);
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len() / 2).map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).expect("hex")).collect()
}

// the wire: what roam_core sends the hub reads, and its PEERS lists decode in roam_core
#[test]
fn the_wire_what_roam_core_sends_the_hub_reads_and_its_peers_lists_decode_in_roam_core() {
    let deps = repo().join("deps");
    let wasi = std::fs::read_dir(&deps).ok().and_then(|d| {
        let mut v: Vec<PathBuf> =
            d.flatten().filter(|e| e.file_name().to_string_lossy().starts_with("wasi-sdk-")).map(|e| e.path()).collect();
        v.sort();
        v.pop()
    });
    let Some(wasi) = wasi else {
        eprintln!("no wasi-sdk in deps/: skipped");
        return;
    };
    if !have_node() {
        eprintln!("no node: skipped");
        return;
    }
    let w = Path::new(env!("CARGO_TARGET_TMPDIR")).join("roam_hubwire.wasm");
    let out = Command::new(wasi.join("bin/clang"))
        .current_dir(repo())
        .args(["--target=wasm32-wasip1-threads", "-pthread", "-O2", "-ffp-contract=off", "-Iruntime", "-o"])
        .arg(&w)
        .args(["web/test/roam_hubwire.c", "runtime/roam_core.c"])
        .arg("-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=268435456")
        .output()
        .expect("clang");
    assert!(out.status.success(), "clang: {}", String::from_utf8_lossy(&out.stderr));
    let run = |args: &[&str]| -> String {
        let o = Command::new("node").arg(repo().join("web/test/wasi-run.mjs")).arg(&w).args(args).output().expect("node");
        String::from_utf8_lossy(&o.stdout).trim().to_string()
    };
    let (h, s, b) = (unhex(&run(&["hello"])), unhex(&run(&["snap"])), unhex(&run(&["bye"])));
    // the C core's version (roam_core.h RC_CORE_VER), which its HELLO says
    let hdr = std::fs::read_to_string(repo().join("runtime/roam_core.h")).expect("roam_core.h");
    let core: u16 = hdr
        .lines()
        .find_map(|l| l.strip_prefix("#define RC_CORE_VER"))
        .and_then(|r| r.split_whitespace().next())
        .and_then(|v| v.parse().ok())
        .expect("RC_CORE_VER");
    assert_eq!(
        decode_roam(&h),
        Some(RoamMsg::Hello { sender: 0x1234ABCD, core, status: 2, room: fnv1a("blue"), name: "Ann K.".into(), crews: vec![] })
    );
    assert_eq!(
        hello_with(0x1234ABCD, &HelloOpts { room: fnv1a("blue"), name: b"Ann K.", seen: &[0x11, 0x22], core, ..Default::default() }),
        h,
        "this test's HELLO = the C core's, byte for byte"
    );
    assert_eq!(
        decode_roam(&s),
        Some(RoamMsg::Snap { sender: 0x1234ABCD, status: 2, p: [1695.25, -883.5, 24.125] }),
        "the newest state's position"
    );
    assert_eq!(decode_roam(&b), Some(RoamMsg::Bye { sender: 0x1234ABCD }));
    let mut bad = h.clone();
    bad[20] ^= 1;
    assert_eq!(decode_roam(&bad), None, "a wrong CRC is no datagram");
    let peers = encode_peers(
        0xB0B,
        9,
        &[PeerEntry { vip: ip("10.77.0.5"), port: 47210, id: 3 }, PeerEntry { vip: ip("10.77.0.6"), port: 47300, id: 0xDEADBEEF }],
    );
    assert_eq!(run(&["peers", &hex(&peers)]), format!("ok 2 10.77.0.5:47210:3 10.77.0.6:47300:{}", 0xDEADBEEFu32));
    assert_eq!(run(&["peers", &hex(&encode_peers(0xB0B, 10, &[]))]), "ok 0", "an empty list (the hub is there)");
}

/// web/test/roam_hubwire.c with roam_core.c, built for wasm into `file` (each test its own: they run at once), and
/// the way to run it under node, its output trimmed; None without a wasi-sdk in deps/ or node (the test is skipped)
fn hubwire(file: &str) -> Option<impl Fn(&[&str]) -> String> {
    let wasi = std::fs::read_dir(repo().join("deps")).ok().and_then(|d| {
        let mut v: Vec<PathBuf> =
            d.flatten().filter(|e| e.file_name().to_string_lossy().starts_with("wasi-sdk-")).map(|e| e.path()).collect();
        v.sort();
        v.pop()
    });
    let Some(wasi) = wasi.filter(|_| have_node()) else {
        eprintln!("no wasi-sdk in deps/, or no node: skipped");
        return None;
    };
    let w = Path::new(env!("CARGO_TARGET_TMPDIR")).join(file);
    let out = Command::new(wasi.join("bin/clang"))
        .current_dir(repo())
        .args(["--target=wasm32-wasip1-threads", "-pthread", "-O2", "-ffp-contract=off", "-Iruntime", "-o"])
        .arg(&w)
        .args(["web/test/roam_hubwire.c", "runtime/roam_core.c"])
        .arg("-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=268435456")
        .output()
        .expect("clang");
    assert!(out.status.success(), "clang: {}", String::from_utf8_lossy(&out.stderr));
    Some(move |args: &[&str]| {
        let o = Command::new("node").arg(repo().join("web/test/wasi-run.mjs")).arg(&w).args(args).output().expect("node");
        String::from_utf8_lossy(&o.stdout).trim().to_string()
    })
}

// the wire: a session's HELLO to the hub as roam_core makes it (rc_tick's, fill_hello: the ids of the two players the
// hub listed, who said hello, then its two crews from its config) names its crews; they are the room hashes the page
// makes (players.ts roomHash, players.test.mjs's 0x160fba32 for "Blue Room") and this test's; the hub links that
// player in a crowd with a crew-mate 2 km off (a HELLO made here) first, and not one of another crew
#[test]
fn the_wire_a_sessions_hello_to_the_hub_names_its_crews() {
    let Some(run) = hubwire("roam_hubwire_crew.wasm") else { return };
    let h = unhex(&run(&["hellocrew"]));
    let (blue, owls) = (room_hash("Blue Room"), room_hash("Night Owls"));
    // capitals and blanks do not count, a name of blanks is none
    assert_eq!([blue, room_hash("blueroom"), room_hash(" Blue Room\t"), room_hash("BLUE ROOM")], [0x160F_BA32; 4]);
    assert_eq!((room_hash("night owls"), fnv1a("nightowls")), (owls, owls));
    assert_eq!((room_hash(""), room_hash(" \t\r\n")), (0, 0));
    // the crew flag, the ids it hears (the hub's list), then its crews
    assert!(h.len() > 30 && u16::from_le_bytes([h[14], h[15]]) & HF_CREW != 0, "the crew flag: {}", hex(&h));
    let nl = h[28] as usize;
    let at = |o: usize| u32::from_le_bytes(h[o..o + 4].try_into().unwrap());
    let ids: Vec<u32> = (0..h[29 + nl] as usize).map(|k| at(30 + nl + 4 * k)).collect();
    assert_eq!(ids, [0x11, 0x22], "the ids it hears: {}", hex(&h));
    match decode_roam(&h) {
        Some(RoamMsg::Hello { sender, status, crews, .. }) => assert_eq!((sender, status, crews), (0x1234ABCD, 2, vec![blue, owls])),
        other => panic!("not its HELLO: {other:?} ({})", hex(&h)),
    }
    // that player (V(0)) in a crowd of 60 within 15 m, and V(61) 2 km off in "Night Owls" or in "Red"
    for (crew, want) in [(owls, true), (room_hash("Red"), false)] {
        let mut k = Kit::new(RoamOptions { view_m: 1e-9, ..RoamOptions::default() });
        k.from(V(0), &h);
        k.from(V(0), &snap(0x1234ABCD, [0.25, 0.25, 0.0]));
        for i in 1..=60u32 {
            k.from(V(i), &hello_with(100 + i, &HelloOpts { core: 6, ..Default::default() }));
            k.from(V(i), &snap(100 + i, [((i - 1) % 8) as f64 * 3.0 - 9.25, ((i - 1) / 8) as f64 * 3.0 - 9.25, 0.0]));
        }
        k.from(V(61), &hello_with(161, &HelloOpts { core: 6, crews: Some(&[crew]), ..Default::default() }));
        k.from(V(61), &snap(161, [2000.25, 0.25, 0.0]));
        k.hub.tick();
        let l = Lists(k.lists());
        l.both_ends();
        assert_eq!((l.has(0, 61), l.of(0).len()), (want, 32), "crew {crew:#x}: the crew-mate linked, the links full");
        assert_eq!(k.info("crewLinks"), if want { 1.0 } else { 0.0 });
        assert_eq!(k.hub.in_crews(&[blue, owls]), [1, 1 + want as usize]);
    }
}

// the hub's crew marks as the C core takes them: a session's own HELLO (core 8: it reads the marks, HF_HUBCREW) and
// four players near it, in its "Night Owls", in "Red", in its "Blue Room", in none; the list the hub makes for it,
// given to a session in those two crews, colours each player as the hub marked it (rc_peer_crew), and names no crew but
// the session's own
#[test]
fn the_wire_the_hubs_crew_marks_colour_a_sessions_peers() {
    let Some(run) = hubwire("roam_hubwire_marks.wasm") else { return };
    let h = unhex(&run(&["hellocrew"]));
    assert!(u16::from_le_bytes([h[14], h[15]]) & HF_HUBCREW != 0, "a core 8 HELLO reads the marks: {}", hex(&h));
    let (blue, owls, red) = (room_hash("Blue Room"), room_hash("Night Owls"), room_hash("Red"));
    let mut k = Kit::new(RoamOptions { view_m: 1e-9, ..RoamOptions::default() });
    k.from(V(0), &h);
    k.from(V(0), &snap(0x1234ABCD, [0.25, 0.25, 0.0]));
    let who: [Option<&[u32]>; 4] = [Some(&[owls]), Some(&[red]), Some(&[red, blue]), None];
    for (i, crews) in who.iter().enumerate() {
        let i = i as u32 + 1;
        k.from(V(i), &hello_with(100 + i, &HelloOpts { core: 8, crews: *crews, hubcrew: true, ..Default::default() }));
        k.from(V(i), &snap(100 + i, [i as f64 * 10.0 + 0.25, 0.25, 0.0]));
    }
    k.hub.tick();
    let f = k.sent.lock().iter().find(|(vip, _)| *vip == V(0)).map(|(_, f)| f.clone()).expect("the session's list");
    assert_eq!(crew_marks(&f).map(|m| m.0), Some(vec![blue, owls]), "its own crews, and no other: {}", hex(&f[16..]));
    let want = |e: &PeerEntry| match (0..4).find(|&i| V(i + 1) == e.vip) {
        Some(0) => 1,
        Some(2) => 0,
        _ => -1,
    };
    let list = peers_of(&f);
    assert_eq!(list.len(), 4);
    let expect: Vec<String> = list
        .iter()
        .map(|e| format!("{}.{}.{}.{}:{}:{}", e.vip >> 24, e.vip >> 16 & 255, e.vip >> 8 & 255, e.vip & 255, e.port, want(e)))
        .collect();
    assert_eq!(run(&["hubcrew", &hex(&f[16..])]), format!("ok {}", expect.join(" ")), "the session's crews by the hub's marks");
    assert!(!f.windows(4).any(|b| b == red.to_le_bytes()), "no crew of another player's on its list");
}

/// the lists by player i (10.77.0.(10 + i), port 47210)
struct Lists(std::collections::HashMap<String, Vec<PeerEntry>>);

impl Lists {
    fn of(&self, i: u32) -> &[PeerEntry] {
        self.0.get(&format!("{}:47210", V(i))).map_or(&[], |l| l.as_slice())
    }
    fn of_vip(&self, vip: u32) -> &[PeerEntry] {
        self.0.get(&format!("{vip}:47210")).map_or(&[], |l| l.as_slice())
    }
    fn has(&self, i: u32, j: u32) -> bool {
        self.of(i).iter().any(|e| e.vip == V(j))
    }
    /// every link listed at both ends
    fn both_ends(&self) {
        for (k, l) in &self.0 {
            for e in l {
                assert!(self.of_vip(e.vip).iter().any(|x| format!("{}:47210", x.vip) == *k), "{k}: its links list it too");
            }
        }
    }
}

// links of players of core version 1 (every pair | the grid): the nearest, both ends, at most 7 a player, only in
// free roam lately, rooms apart
#[test]
fn links_of_players_of_core_version_1_the_nearest_both_ends_at_most_7_only_in_free_roam_lately_rooms_apart() {
    for (how, o) in ways() {
        let mut k = Kit::new(o);
        // 20 players on a line 10 m apart, one far away, two in another room, one with no position yet
        for i in 0..20 {
            k.from(V(i), &hello(100 + i));
            k.from(V(i), &snap(100 + i, [i as f64 * 10.0, 0.0, 0.0]));
        }
        k.from(V(20), &hello(120));
        k.from(V(20), &snap(120, [5000.0, 0.0, 0.0]));
        for i in [21, 22] {
            k.from(V(i), &hello_with(100 + i, &HelloOpts { room: fnv1a("red"), ..Default::default() }));
            k.from(V(i), &snap(100 + i, [i as f64, 0.0, 0.0]));
        }
        k.from(V(23), &hello(123));
        k.hub.tick();
        let l = Lists(k.lists());
        assert_eq!(l.0.len(), 24, "{how}: every player gets a list, an empty one too");
        for i in 0..24 {
            assert!(l.of(i).len() <= 7, "{how}: player {i}: at most 7 ({})", l.of(i).len());
        }
        l.both_ends();
        assert!(l.has(0, 1) && l.has(10, 9) && l.has(10, 11), "{how}: neighbours on the line are linked");
        for i in 5..15 {
            for e in l.of(i) {
                assert!((e.vip as i64 - V(i) as i64).abs() <= 4, "{how}: player {i}: its links are the nearest");
            }
        }
        assert!(l.has(19, 20) || !l.of(20).is_empty(), "{how}: the far one is linked to somebody");
        assert!(l.has(21, 22) && l.of(21).len() == 1, "{how}: another room: only each other");
        assert_eq!(l.of(23).len(), 0, "{how}: no position yet: no links");
        for i in 0..20 {
            for j in 0..20 {
                if i != j && l.of(i).len() < 7 && l.of(j).len() < 7 {
                    assert!(l.has(i, j), "{how}: {i} and {j} both have room, so they are linked");
                }
            }
        }
        // a position older than 60 s: no links (one in a shop keeps them until then)
        k.clear();
        k.step(30_000.0);
        for i in 1..23 {
            k.from(V(i), &hello(100 + i));
            k.from(V(i), &snap(100 + i, if i == 1 { [10.0, 0.0, 0.0] } else { [i as f64 * 10.0, 0.0, 0.0] }));
        }
        k.from(V(0), &hello(100));
        k.hub.tick();
        assert!(!Lists(k.lists()).of(0).is_empty(), "{how}: 30 s without a SNAP: still linked");
        k.clear();
        k.step(31_000.0);
        for i in 0..23 {
            k.from(V(i), &hello(100 + i));
        }
        for i in 1..23 {
            k.from(V(i), &snap(100 + i, [i as f64 * 10.0, 0.0, 0.0]));
        }
        k.hub.tick();
        assert_eq!(Lists(k.lists()).of(0).len(), 0, "{how}: 61 s without a SNAP: no links");
    }
}

// links of players of core version 2 (every pair | the grid): at most 32 a player, 7 for one of version 1 among them
#[test]
fn links_of_players_of_core_version_2_at_most_32_a_player_7_for_one_of_version_1_among_them() {
    for (how, o) in ways() {
        let mut k = Kit::new(o);
        // 50 players on a line 10 m apart, player 15 of core version 1
        for i in 0..50 {
            k.from(V(i), &hello_with(100 + i, &HelloOpts { core: if i == 15 { 1 } else { 2 }, ..Default::default() }));
            k.from(V(i), &snap(100 + i, [i as f64 * 10.0, 0.0, 0.0]));
        }
        k.hub.tick();
        let l = Lists(k.lists());
        let deg = |i: u32| if i == 15 { 7 } else { 32 };
        for i in 0..50 {
            assert!(l.of(i).len() <= deg(i), "{how}: player {i}: at most {} ({})", deg(i), l.of(i).len());
        }
        l.both_ends();
        assert_eq!(l.of(15).len(), 7, "{how}: the version 1 player: 7, as its group of 8 holds");
        for i in 0..50 {
            for j in 0..50 {
                if i != j && l.of(i).len() < deg(i) && l.of(j).len() < deg(j) {
                    assert!(l.has(i, j), "{how}: {i} and {j} both have room, so they are linked");
                }
            }
        }
        assert!(l.of(25).len() > 16, "{how}: more than 16 for a player of version 2 ({})", l.of(25).len());
    }
}

// links stay put at the edge, players come and go (every pair | the grid)
#[test]
fn links_stay_put_at_the_edge_players_come_and_go() {
    for (how, o) in ways() {
        let mut k = Kit::new(RoamOptions { degree: 1, ..o });
        // A at 0, B at 100: linked; C comes 10 m from A: a link stays its first 5 s
        let at = |k: &mut Kit, i: u32, x: f64| {
            k.from(V(i), &hello(200 + i));
            k.from(V(i), &snap(200 + i, [x, 0.0, 0.0]));
        };
        let first = |k: &Kit| Lists(k.lists()).of(0)[0].vip;
        at(&mut k, 0, 0.0);
        at(&mut k, 1, 100.0);
        k.hub.tick();
        assert_eq!(first(&k), V(1), "{how}");
        k.clear();
        k.step(1000.0);
        at(&mut k, 0, 0.0);
        at(&mut k, 1, 100.0);
        at(&mut k, 2, -10.0);
        k.hub.tick();
        assert_eq!(first(&k), V(1), "{how}: a link stays at least 5 s");
        // after that it counts as half its distance: 60 m does not beat a linked 100 m, 40 m does
        k.clear();
        k.step(5000.0);
        at(&mut k, 0, 0.0);
        at(&mut k, 1, 100.0);
        at(&mut k, 2, -60.0);
        k.hub.tick();
        assert_eq!(first(&k), V(1), "{how}: 60 m does not beat a linked 100 m");
        k.clear();
        k.step(1000.0);
        at(&mut k, 0, 0.0);
        at(&mut k, 1, 100.0);
        at(&mut k, 2, -40.0);
        k.hub.tick();
        assert_eq!(first(&k), V(2), "{how}: under half the distance: the link moves");
        // BYE, silence, a departure from the switch
        assert_eq!(k.hub.vips(), HashSet::from([V(0), V(1), V(2)]), "{how}: the browsers in free roam (the server's console)");
        k.from(V(2), &bye(202));
        assert_eq!(k.info("players"), 2.0, "{how}: BYE: forgotten at once");
        k.step(7000.0);
        at(&mut k, 0, 0.0);
        k.hub.tick();
        assert_eq!(k.info("players"), 1.0, "{how}: 6 s without a word: forgotten");
        assert_eq!(k.hub.vips(), HashSet::from([V(0)]));
        k.hub.peer_gone(V(0));
        assert_eq!(k.info("players"), 0.0, "{how}: left the switch: forgotten");
    }
}

// limits: two endpoints a browser, a player starts with its HELLO, bad datagrams counted
#[test]
fn limits_two_endpoints_a_browser_a_player_starts_with_its_hello_bad_datagrams_counted() {
    let mut k = Kit::new(RoamOptions::default());
    for port in [47210u16, 47211, 47212] {
        k.hub.frame(V(0), &frame_ports(V(0), &hello(300 + port as u32), port, 47210));
    }
    assert_eq!(k.info("players"), 2.0);
    assert_eq!(k.info("refused"), 1.0);
    k.from(V(1), &snap(301, [0.0, 0.0, 0.0]));
    assert_eq!(k.info("players"), 2.0, "a SNAP before any HELLO: nobody new");
    k.hub.frame(V(1), b"nonsense, not a frame at all");
    let mut f = frame(V(1), &hello(301));
    f[30] ^= 0xff;
    k.hub.frame(V(1), &f);
    assert_eq!(k.info("bad"), 2.0);
    let far = frame(V(1), &snap(301, [1e6, 0.0, 0.0]));
    k.from(V(1), &hello(301));
    k.hub.frame(V(1), &far);
    assert_eq!(k.info("bad"), 3.0, "a position out of the map: refused");
}

// in free roam (/players' roam): the browsers driving there or paused there, as their last HELLO or SNAP says, each
// once whatever its endpoints; in a race, a shop, the menus or loading, not
#[test]
fn in_free_roam_driving_or_paused_each_browser_once() {
    let mut k = Kit::new(RoamOptions::default());
    // roam_core.h RC_ST_*: 0 menus, 1 loading, 2 free roam, 3 a race, 4 a shop, 5 paused
    for st in 0..6u32 {
        k.from(V(st), &hello_with(400 + st, &HelloOpts { status: st as u8, ..Default::default() }));
    }
    assert_eq!(k.hub.in_roam(), 2, "free roam, and paused there");
    // another start of the game in the browser in free roam: a second endpoint
    k.hub.frame(V(2), &frame_ports(V(2), &hello(500), 47211, 47210));
    assert_eq!(k.hub.in_roam(), 2, "a browser once");
    k.from(V(5), &snap_n(405, [0.0, 0.0, 0.0], 3, 1));
    assert_eq!(k.hub.in_roam(), 1, "a SNAP says the paused one went into a race");
    k.from(V(0), &hello(400));
    assert_eq!(k.hub.in_roam(), 2, "out of the menus into free roam");
    k.from(V(2), &bye(402));
    assert_eq!(k.hub.in_roam(), 2, "BYE from one endpoint: its other one is in free roam");
    k.hub.peer_gone(V(2));
    assert_eq!(k.hub.in_roam(), 1, "left the switch");
}

// /players' room: in free roam by room, for the rooms with a name (room 0, none, is not counted), each browser once
#[test]
fn in_free_roam_by_room() {
    let mut k = Kit::new(RoamOptions::default());
    let (blue, red) = (fnv1a("blue"), fnv1a("red"));
    k.from(V(1), &hello_with(601, &HelloOpts { room: blue, ..Default::default() }));
    k.from(V(2), &hello_with(602, &HelloOpts { room: blue, status: 5, ..Default::default() }));
    k.from(V(3), &hello_with(603, &HelloOpts { room: blue, status: 3, ..Default::default() }));
    k.from(V(4), &hello_with(604, &HelloOpts { room: red, ..Default::default() }));
    k.from(V(5), &hello(605));
    k.hub.frame(V(1), &frame_ports(V(1), &hello_with(606, &HelloOpts { room: blue, ..Default::default() }), 47211, 47210));
    let rooms = k.hub.in_rooms();
    assert_eq!((rooms.get(&blue), rooms.get(&red), rooms.len()), (Some(&2), Some(&1), 2), "driving or paused, a browser once: {rooms:?}");
    assert_eq!(k.hub.in_roam(), 4);
}

// a player that comes and goes, HELLO and BYE over and over (a client may send the hub 2500 frames a second), costs the
// hub little: a player gone is taken out of the others' links once in a while, for many at once, not with every BYE
// (each walked every player's links: 2000 players' 64,000)
#[test]
fn a_player_that_comes_and_goes_costs_the_hub_little() {
    let mut k = Kit::new(RoamOptions::default());
    let v = |i: u32| 0x0A4D_0100 + i;
    let crowd = |k: &mut Kit| {
        for i in 0..2000u32 {
            k.from(v(i), &hello_with(1000 + i, &HelloOpts { core: 2, ..Default::default() }));
            k.from(v(i), &snap(1000 + i, [(i % 50) as f64 * 20.0, (i / 50) as f64 * 20.0, 0.0]));
        }
    };
    crowd(&mut k);
    let t0 = Instant::now();
    k.hub.tick();
    let tick = t0.elapsed();
    let links = k.info("links");
    assert!(links > 30_000.0, "{links}");
    let x = v(3000);
    let t0 = Instant::now();
    for n in 0..2000u32 {
        let port = 47210 + (n % 2) as u16;
        k.hub.frame(x, &frame_ports(x, &hello(50_000 + n), port, 47210));
        if n % 7 == 3 {
            // links of the crowd's own come and go too: Ann of the crowd leaves and is back at once
            k.from(v(n % 2000), &bye(1000 + n % 2000));
            k.from(v(n % 2000), &hello_with(1000 + n % 2000, &HelloOpts { core: 2, ..Default::default() }));
        }
        k.hub.frame(x, &frame_ports(x, &bye(50_000 + n), port, 47210));
    }
    let churn = t0.elapsed();
    println!("2000 comings and goings: {churn:?}; a tick of 2000 players: {tick:?}");
    assert!(churn < tick, "2000 comings and goings took {churn:?}, longer than a whole tick ({tick:?})");
    // what they left: the crowd, and the links of those that were not back as new players
    assert_eq!(k.info("players"), 2000.0);
    let left: f64 = k.hub.links().iter().map(|(_, l)| l.len() as f64).sum::<f64>() / 2.0;
    assert_eq!(k.info("links"), left, "info() counts the links listed");
    assert!(left < links, "the ones that went are out of the others' links");
    let names: HashSet<String> = k.hub.links().into_iter().map(|(n, _)| n).collect();
    assert!(k.hub.links().iter().all(|(_, l)| l.iter().all(|n| names.contains(n))), "no link to a player gone");
    // and the next tick links the crowd as before
    k.step(1000.0);
    crowd(&mut k);
    k.hub.tick();
    assert!(k.info("links") > 30_000.0);
}

// 64 players of core version 1|2 in one place: each list at most 7|32, the tick stays cheap
#[test]
fn sixty_four_players_in_one_place_each_list_at_most_its_degree_the_tick_stays_cheap() {
    for core in [1u16, 2] {
        let mut k = Kit::new(RoamOptions::default());
        let most = if core == 1 { 7 } else { 32 };
        for i in 0..64u32 {
            let v = ip(&format!("10.77.0.{}", 2 + i));
            k.from(v, &hello_with(400 + i, &HelloOpts { core, ..Default::default() }));
            k.from(v, &snap(400 + i, [(i as f64).cos() * 30.0, (i as f64).sin() * 30.0, 0.0]));
        }
        for _ in 0..20 {
            k.hub.tick();
        }
        let sent = k.sent.lock();
        let l = lists(&sent[sent.len() - 64..]);
        let n: Vec<usize> = l.values().map(|l| l.len()).collect();
        assert!(n.iter().all(|&x| x <= most) && n.iter().filter(|&&x| x == most).count() >= 60, "lists of {most} ({n:?})");
        println!("64 players of core version {core}: {} links, tick at most {:.2} ms", k.info("links"), k.info("tickMsMax"));
        assert!(k.info("tickMsMax") < 50.0);
    }
}

// a panic in a tick (a bug: here the switch's send, halfway through the lists) leaves a hub that goes on: its thread
// catches the panic, and the next ticks link everyone and send every list
#[test]
fn a_panic_in_a_tick_leaves_a_hub_that_goes_on() {
    // the last tick's lists of a hub whose send panics at its `panic_at`-th list (none: never)
    let run = |exact_room: usize, panic_at: Option<usize>| {
        let sent = Arc::new(Mutex::new(Vec::<(u32, Bytes)>::new()));
        let s2 = sent.clone();
        let t = Arc::new(Mutex::new(1_000_000.0));
        let t2 = t.clone();
        let n = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let mut hub = Hub::new(
            RoamOptions { exact_room, ..RoamOptions::default() },
            Box::new(move || *t2.lock()),
            Box::new(|| 0.0),
            Box::new(|_: &str| {}),
            Box::new(move |vip, f| {
                assert!(Some(n.fetch_add(1, Ordering::Relaxed)) != panic_at, "a bug in the middle of a tick");
                s2.lock().push((vip, f));
                true
            }),
            0xB0B,
        );
        let v = |i: u32| 0x0A4D_0100 + i;
        for tick in 0..4 {
            for i in 0..100u32 {
                hub.frame(v(i), &frame(v(i), &hello_with(1000 + i, &HelloOpts { core: 2, ..Default::default() })));
                hub.frame(v(i), &frame(v(i), &snap(1000 + i, [(i % 10) as f64 * 15.0, (i / 10) as f64 * 15.0, 0.0])));
            }
            let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| hub.tick()));
            assert_eq!(r.is_err(), panic_at.is_some() && tick == 1, "the second tick's 51st list panics, nothing else");
            *t.lock() += 1000.0;
        }
        assert_eq!(hub.info()["players"], 100);
        let sent = sent.lock();
        lists(&sent[sent.len() - 100..])
    };
    for exact_room in [1000, 1] {
        let last = run(exact_room, Some(150));
        assert_eq!(last.len(), 100, "every player's list");
        let by_vip: std::collections::HashMap<u32, &Vec<PeerEntry>> =
            last.iter().map(|(k, l)| (k.split(':').next().and_then(|x| x.parse().ok()).unwrap_or(0), l)).collect();
        for (a, l) in &by_vip {
            assert!(l.len() <= 32 && l.iter().all(|e| by_vip[&e.vip].iter().any(|x| x.vip == *a)), "at most 32, both ends");
        }
        let open: Vec<u32> = by_vip.iter().filter(|(_, l)| l.len() < 32).map(|(a, _)| *a).collect();
        for a in &open {
            assert!(open.iter().all(|b| a == b || by_vip[a].iter().any(|e| e.vip == *b)), "two with room are linked");
        }
        assert_eq!(last, run(exact_room, None), "the same lists as a hub that never panicked");
    }
}

// a list waiting in a slow player's queue keeps a block of 4 KiB at most (its own 357 bytes and its neighbours'), not
// every list of its tick (2000 players' lists were one buffer of 700 KB, kept whole for as long as any of them waited)
#[test]
fn a_list_waiting_holds_little() {
    // (the lattice's links by distance alone: player 7 has 32)
    let mut k = Kit::new(ways()[0].1.clone());
    for i in 0..300u32 {
        let v = 0x0A4D_0100 + i;
        k.from(v, &hello_with(1000 + i, &HelloOpts { core: 2, ..Default::default() }));
        k.from(v, &snap(1000 + i, [(i % 20) as f64 * 10.0, (i / 20) as f64 * 10.0, 0.0]));
    }
    k.hub.tick();
    let one = k.sent.lock()[7].1.clone();
    k.clear();
    let len = one.len();
    assert_eq!(len, 16 + 16 + 1 + 10 * 32 + 4, "a list of 32");
    // (what it keeps: the rest of its block, from where it starts)
    let mine = one.try_into_mut().map_err(|_| "its block is in other hands").expect("no other list holds its block");
    assert!(mine.capacity() <= 4096, "it keeps {} bytes for a list of {len}", mine.capacity());
}

// a busy server: every player gets fewer links, and more again once it has room (every pair | the grid): the cap on
// the links a quarter down at most every 5 s to 8, nothing between 60 and 85 %, 2 up every 15 s
#[test]
fn a_busy_server_every_player_gets_fewer_links_and_more_again_once_it_has_room() {
    for (way, o) in ways() {
        let mut k = Kit::new(o);
        *k.busy.lock() = 0.95;
        // one second of the 40 players: the lists checked, then the cap and whether free roam counts as busy
        let second = |k: &mut Kit| -> (usize, bool) {
            k.clear();
            for i in 0..40u32 {
                let v = ip(&format!("10.77.0.{}", 2 + i));
                k.from(v, &hello_with(500 + i, &HelloOpts { core: 2, ..Default::default() }));
                k.from(v, &snap(500 + i, [(i as f64).cos() * 30.0, (i as f64).sin() * 30.0, 0.0]));
            }
            k.hub.tick();
            k.step(1000.0);
            let cap = k.info("cap") as usize;
            let l = Lists(k.lists());
            let n: Vec<usize> = l.0.values().map(|l| l.len()).collect();
            assert!(
                n.iter().all(|&x| x <= cap) && n.iter().sum::<usize>() as f64 >= 0.9 * cap as f64 * n.len() as f64,
                "{way}: lists of {cap} ({n:?})"
            );
            l.both_ends();
            let open: Vec<(&String, &Vec<PeerEntry>)> = l.0.iter().filter(|(_, l)| l.len() < cap).collect();
            for (a, la) in &open {
                for (b, _) in &open {
                    if a != b {
                        assert!(la.iter().any(|x| format!("{}:47210", x.vip) == **b), "{way}: two with room left: linked");
                    }
                }
            }
            (cap, k.hub.crowded())
        };
        let (mut caps, mut crowded) = (Vec::new(), Vec::new());
        let seconds = |k: &mut Kit, n: usize, caps: &mut Vec<usize>, crowded: &mut Vec<bool>| {
            for _ in 0..n {
                let (c, b) = second(k);
                caps.push(c);
                crowded.push(b);
            }
        };
        seconds(&mut k, 30, &mut caps, &mut crowded);
        assert_eq!([caps[0], caps[4], caps[5], caps[10], caps[15], caps[20], caps[29]], [24, 24, 18, 13, 9, 8, 8], "{way}: {caps:?}");
        *k.busy.lock() = 0.7;
        seconds(&mut k, 30, &mut caps, &mut crowded);
        assert!(caps[30..60].iter().all(|&c| c == 8), "{way}: between busyLow and busyHigh: as it is");
        *k.busy.lock() = 0.2;
        seconds(&mut k, 31, &mut caps, &mut crowded);
        assert_eq!([caps[60], caps[74], caps[75], caps[90]], [10, 10, 12, 14], "{way}: {:?}", &caps[60..]);
        {
            let lines = k.lines.lock();
            assert_eq!(lines.len(), 8, "{lines:?}");
            assert_eq!(lines[0], "[roam] the server's thread was 95 % busy: at most 24 links a player (was 32)");
        }
        assert_eq!(k.info("capCuts"), 5.0);
        // busy for the start page's question (/players roamBusy) while the cap is under 24: from 18 down, until it is
        // 24 again
        seconds(&mut k, 90, &mut caps, &mut crowded);
        assert!(*caps.last().unwrap() > 24, "{way}: {:?}", &caps[91..]);
        assert!(crowded.iter().zip(&caps).all(|(&c, &cap)| c == (cap < 24)), "{way}: {caps:?} {crowded:?}");
        let last_busy = crowded.iter().rposition(|&c| c).unwrap();
        let back = 60 + caps[60..].iter().position(|&c| c == 24).unwrap();
        assert_eq!((crowded[4], crowded[5], last_busy + 1 == back), (false, true, true), "{way}");
    }
}

// 600 players in a city | a district | a crowd: the grid links as every pair does: a room larger than `exact_room`,
// the grid's links against every pair's, for the same players (some of core version 1; two rooms; a few far out):
// the same rules (at most the degree, both ends, no two with room left unlinked) and about the same distances
#[test]
fn six_hundred_players_the_grid_links_as_every_pair_does() {
    for (where_, w) in [("a city", 8000.0), ("a district", 1500.0), ("a crowd", 100.0)] {
        let mut seed: u32 = 12345;
        let mut rnd = || {
            seed = seed.wrapping_mul(1103515245).wrapping_add(12345);
            seed as f64 / 4294967296.0
        };
        struct Q {
            v: u32,
            id: u32,
            core: u16,
            room: u32,
            p: [f64; 3],
        }
        let players: Vec<Q> = (0..600u32)
            .map(|i| Q {
                v: ip(&format!("10.77.{}.{}", i & 7, 2 + (i >> 3))),
                id: 700 + i,
                core: if i % 50 == 7 { 1 } else { 2 },
                room: if i % 10 == 3 { fnv1a("red") } else { 0 },
                p: if i % 97 == 5 {
                    [15000.0 - i as f64, -15000.0, 0.0]
                } else {
                    let x = rnd() * w - w / 2.0;
                    let y = rnd() * w - w / 2.0;
                    [x, y, rnd() * 20.0]
                },
            })
            .collect();
        let run = |o: RoamOptions| {
            let mut k = Kit::new(o);
            for _ in 0..3 {
                k.clear();
                for q in &players {
                    k.from(q.v, &hello_with(q.id, &HelloOpts { core: q.core, room: q.room, ..Default::default() }));
                    k.from(q.v, &snap(q.id, q.p));
                }
                k.hub.tick();
                k.step(1000.0);
            }
            (Lists(k.lists()), k.info("tickMsMax"))
        };
        let (all, all_ms) = run(RoamOptions { exact_room: 1000, ..RoamOptions::default() });
        let (grid, grid_ms) = run(RoamOptions::default());
        let at: std::collections::HashMap<u32, &Q> = players.iter().map(|q| (q.v, q)).collect();
        let d = |a: &Q, b: &Q| ((a.p[0] - b.p[0]).powi(2) + (a.p[1] - b.p[1]).powi(2) + (a.p[2] - b.p[2]).powi(2)).sqrt();
        let sum = |l: &Lists| {
            let (mut n, mut m) = (0usize, 0.0);
            for q in &players {
                let most = if q.core == 1 { 7 } else { 32 };
                let lq = l.of_vip(q.v);
                assert!(lq.len() <= most, "at most {most} ({})", lq.len());
                for e in lq {
                    assert!(l.of_vip(e.vip).iter().any(|x| x.vip == q.v), "both ends");
                    assert_eq!(at[&e.vip].room, q.room, "one room");
                    n += 1;
                    m += d(q, at[&e.vip]);
                }
            }
            let open: Vec<&Q> = players.iter().filter(|q| l.of_vip(q.v).len() < if q.core == 1 { 7 } else { 32 }).collect();
            for a in &open {
                for b in &open {
                    if a.v != b.v && a.room == b.room {
                        assert!(l.of_vip(a.v).iter().any(|e| e.vip == b.v), "two with room are linked");
                    }
                }
            }
            (n as f64 / 2.0, m / n as f64)
        };
        let (a_links, a_mean) = sum(&all);
        let (g_links, g_mean) = sum(&grid);
        println!(
            "600 players in {w} m ({where_}): every pair {a_links} links of {a_mean:.1} m ({all_ms:.0} ms), the grid {g_links} of {g_mean:.1} m ({grid_ms:.0} ms)"
        );
        assert!((g_links - a_links).abs() <= a_links * 0.01, "as many links");
        assert!(g_mean <= a_mean * 1.03, "as near");
    }
}

// ---- the hub on its thread (the hub's part of 'on the switch: frames to 10.77.0.253 reach the hub, its lists come
// back, a departure is forgotten') ----

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

// on its thread in the server's runtime, the hub's busy measure is the runtime's workers' (they run the switch): with
// all of them busy the cap on the links comes down, few of the machine's cores as they may be (the process's CPU time
// over every core read 2 busy workers of 24 cores as 8 %, and the cap never moved)
#[test]
fn on_its_thread_busy_workers_cut_the_links() {
    let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build().expect("a runtime");
    let lines = Arc::new(Mutex::new(Vec::<String>::new()));
    let l2 = lines.clone();
    let log = Arc::new(move |s: &str| l2.lock().push(s.into()));
    let hub = rt.block_on(async { RoamHub::spawn(RoamOptions { tick_ms: 250, ..RoamOptions::default() }, log) });
    let stop = Arc::new(AtomicBool::new(false));
    for _ in 0..2 {
        let stop = stop.clone();
        rt.spawn(async move {
            while !stop.load(Ordering::Relaxed) {
                let t = Instant::now();
                while t.elapsed() < Duration::from_millis(1) {
                    std::hint::spin_loop();
                }
                tokio::task::yield_now().await;
            }
        });
    }
    let cut = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| wait_for("the cap cut", || !lines.lock().is_empty())));
    stop.store(true, Ordering::Relaxed);
    hub.stop();
    assert!(cut.is_ok(), "both workers busy, and the cap stayed");
    let first = lines.lock()[0].clone();
    assert!(first.ends_with("% busy: at most 24 links a player (was 32)"), "{first}");
}

#[test]
fn on_its_thread_a_hub_given_up_ends_its_thread() {
    let hub = RoamHub::spawn(RoamOptions::default(), Arc::new(|_: &str| {}));
    let rx = hub.info();
    drop(hub);
    // what was posted before still has its answer
    assert!(rx.blocking_recv().is_ok());
}
