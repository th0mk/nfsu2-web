//! Race lobbies (roam_core.h RC_MSG_LOBBY, RC_MSG_LOBBIES): a host's advert to the hub, the open lobbies of a room
//! with the world map's answer to a player that takes them, and the links a lobby needs first.

mod kit;

use std::path::{Path, PathBuf};
use std::process::Command;

use kit::*;
use nfsu2_roam::RoamOptions;

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../..")
}

/// browser i's address: 10.77.0.2 and on
#[allow(non_snake_case)]
fn W(i: u32) -> u32 {
    0x0A4D_0002 + i
}

/// a datagram's flags set (and its CRC again)
fn with_flags(mut d: Vec<u8>, flags: u16) -> Vec<u8> {
    d[14..16].copy_from_slice(&flags.to_le_bytes());
    let len = d.len();
    let crc = crc32fast::hash(&d[..len - 4]);
    d[len - 4..].copy_from_slice(&crc.to_le_bytes());
    d
}

/// a player of core version 5 in `room`, with the lobby flag or without
fn join(k: &mut Kit, i: u32, room: &str, lobbies: bool) {
    let h = hello_with(100 + i, &HelloOpts { core: 5, room: fnv1a(room), ..Default::default() });
    k.from(W(i), &with_flags(h, if lobbies { 0x0080 } else { 0 }));
}

/// a LOBBY datagram as roam_core.c lays it out (op arg state max, to, lobby, race, x y, t, n, reserved, ids)
#[allow(clippy::too_many_arguments)]
fn lobby(sender: u32, op: u8, to: u32, id: u32, race: u32, x: i16, y: i16, state: u8, max: u8, ids: &[u32]) -> Vec<u8> {
    let mut p = vec![0u8; 24 + 4 * ids.len()];
    p[0] = op;
    p[2] = state;
    p[3] = max;
    p[4..8].copy_from_slice(&to.to_le_bytes());
    p[8..12].copy_from_slice(&id.to_le_bytes());
    p[12..16].copy_from_slice(&race.to_le_bytes());
    p[16..18].copy_from_slice(&x.to_le_bytes());
    p[18..20].copy_from_slice(&y.to_le_bytes());
    p[22] = ids.len() as u8;
    for (i, v) in ids.iter().enumerate() {
        p[24 + 4 * i..28 + 4 * i].copy_from_slice(&v.to_le_bytes());
    }
    dgram(12, sender, &p)
}

/// the LOBBIES datagrams the hub sent since the last clear: (to, req, [(x, y, race, n, max, state)])
type Lobbies = (u32, u16, Vec<(i16, i16, u32, u8, u8, u8)>);
fn lobbies(k: &Kit) -> Vec<Lobbies> {
    k.sent
        .lock()
        .iter()
        .filter(|(_, f)| f[16 + 5] == 13)
        .map(|(vip, f)| {
            let d = &f[16..];
            let n = d[18] as usize;
            let e = (0..n)
                .map(|i| {
                    let o = 19 + 11 * i;
                    let race = u32::from_le_bytes([d[o + 4], d[o + 5], d[o + 6], d[o + 7]]);
                    (i16::from_le_bytes([d[o], d[o + 1]]), i16::from_le_bytes([d[o + 2], d[o + 3]]), race, d[o + 8], d[o + 9], d[o + 10])
                })
                .collect();
            (*vip, u16::from_le_bytes([d[16], d[17]]), e)
        })
        .collect()
}

// the hosts' adverts are kept per player; a player that takes lobbies gets its room's with its world map, nearest
// first; one that does not, or of another room, does not; a closed lobby, a stale advert or a bad one is not listed
#[test]
fn the_world_map_brings_the_rooms_open_lobbies() {
    let mut k = Kit::new(RoamOptions { degree: 1, ..RoamOptions::default() });
    join(&mut k, 0, "", true);
    join(&mut k, 1, "", true);
    join(&mut k, 2, "", true);
    join(&mut k, 3, "", false);
    join(&mut k, 4, "red", true);
    for i in 0..5 {
        k.from(W(i), &snap(100 + i, [i as f64 * 100.0, 0.0, 0.0]));
    }
    // 1 and 2 host (2 nearer to 0), 4 hosts in another room; to a player, not the hub: not kept
    k.from(W(1), &lobby(101, 1, 0, 0xA1, 0x1111, 500, 0, 0, 6, &[101]));
    k.from(W(2), &lobby(102, 1, 0, 0xA2, 0x2222, 50, -20, 1, 4, &[102, 103]));
    k.from(W(4), &lobby(104, 1, 0, 0xA4, 0x4444, 0, 0, 0, 6, &[104]));
    k.from(W(3), &lobby(103, 1, 100, 0xA3, 0x3333, 0, 0, 0, 6, &[103]));
    // a bad one: the same player twice
    k.from(W(3), &lobby(103, 1, 0, 0xA5, 0x5555, 0, 0, 0, 6, &[103, 103]));
    k.hub.tick();
    k.clear();
    k.from(W(0), &mapreq(100, 7));
    assert_eq!(lobbies(&k), [(W(0), 7, vec![(50, -20, 0x2222, 2, 4, 1), (500, 0, 0x1111, 1, 6, 0)])]);
    // without the flag: the map, no lobbies; another room: its own
    k.clear();
    k.from(W(3), &mapreq(103, 1));
    assert!(lobbies(&k).is_empty() && !k.sent.lock().is_empty(), "a player that takes none gets its map only");
    k.clear();
    k.from(W(4), &mapreq(104, 1));
    assert_eq!(lobbies(&k), [(W(4), 1, vec![(0, 0, 0x4444, 1, 6, 0)])]);
    // 1 closes its lobby (another id closes nothing), 2's advert goes stale: none listed
    k.from(W(2), &lobby(102, 5, 0, 0xFF, 0x2222, 50, -20, 1, 4, &[]));
    k.from(W(1), &lobby(101, 5, 0, 0xA1, 0x1111, 500, 0, 0, 6, &[]));
    k.step(1100.0);
    k.clear();
    k.from(W(0), &mapreq(100, 8));
    assert_eq!(lobbies(&k), [(W(0), 8, vec![(50, -20, 0x2222, 2, 4, 1)])], "only 2's, closed with another id");
    k.step(3100.0);
    k.from(W(0), &snap(100, [0.0, 0.0, 0.0]));
    k.clear();
    k.from(W(0), &mapreq(100, 9));
    assert_eq!(lobbies(&k), [(W(0), 9, vec![])], "a stale advert is a lobby gone");
}

// a host the player blocks by name (any case: both are "p" here) is not on its list (`Hub::blocks`)
#[test]
fn a_blocked_hosts_lobby_is_not_listed() {
    let mut k = Kit::new(RoamOptions::default());
    join(&mut k, 0, "", true);
    join(&mut k, 1, "", true);
    for i in 0..2 {
        k.from(W(i), &snap(100 + i, [i as f64 * 100.0, 0.0, 0.0]));
    }
    k.from(W(1), &lobby(101, 1, 0, 0xA1, 0x1111, 500, 0, 0, 6, &[101]));
    k.clear();
    k.from(W(0), &mapreq(100, 7));
    assert_eq!(lobbies(&k), [(W(0), 7, vec![(500, 0, 0x1111, 1, 6, 0)])]);
    k.hub.blocks(W(0), vec!["P".into()]);
    k.step(1100.0);
    k.clear();
    k.from(W(0), &mapreq(100, 8));
    assert_eq!(lobbies(&k), [(W(0), 8, vec![])]);
}

// carl hosts a lobby at its start with anna in it; Bob, whom anna blocks, stops at the start: Bob is not linked with
// carl (the link Bob would hear of the lobby and join it by, and race anna), and neither the lobby nor carl is on
// Bob's map; dave is linked with all three and sees the lobby.  Once anna is out of the lobby, Bob is linked with carl
// again.  And the other way round: Bob in the lobby, anna at the start.  (every pair | the grid)
#[test]
fn a_lobby_is_not_there_for_one_kept_apart_from_its_players() {
    let link = |k: &Kit, i: u32, j: u32| k.lists().get(&format!("{}:47210", W(i))).is_some_and(|l| l.iter().any(|e| e.vip == W(j)));
    for (way, o) in ways() {
        for (member, outsider) in [(1u32, 2u32), (2, 1)] {
            let mut k = Kit::new(o.clone());
            for (i, name, x) in [(0u32, &b"carl"[..], 0.0), (1, b"anna", 4.0), (2, b"Bob", 8.0), (3, b"dave", 12.0)] {
                let h = hello_with(100 + i, &HelloOpts { core: 5, name, ..Default::default() });
                k.from(W(i), &with_flags(h, 0x0080));
                k.from(W(i), &snap(100 + i, [x, 0.0, 0.0]));
            }
            k.hub.blocks(W(1), vec!["bob".into()]);
            k.from(W(0), &lobby(100, 1, 0, 0xC1, 0x1111, 0, 0, 0, 6, &[100, 100 + member]));
            k.hub.tick();
            assert!(link(&k, 0, member) && !link(&k, 0, outsider) && !link(&k, outsider, 0), "{way}: not carl and {outsider}");
            assert!(!link(&k, 1, 2) && link(&k, 3, 0) && link(&k, 3, 1) && link(&k, 3, 2), "{way}: dave with all of them");
            k.clear();
            k.from(W(outsider), &mapreq(100 + outsider, 1));
            let map: Vec<_> = k.sent.lock().iter().filter_map(|(_, f)| map_of(f)).flat_map(|m| m.4).collect();
            assert_eq!(lobbies(&k), [(W(outsider), 1, vec![])], "{way}: no lobby on {outsider}'s map");
            assert!(!map.iter().any(|e| e.0 == 0), "{way}: nor carl: {map:?}");
            k.clear();
            k.from(W(3), &mapreq(103, 1));
            assert_eq!(lobbies(&k).first().map(|l| l.2.len()), Some(1), "{way}: dave sees it");
            // the member leaves the lobby
            k.step(600.0);
            k.from(W(0), &lobby(100, 1, 0, 0xC1, 0x1111, 0, 0, 0, 6, &[100]));
            k.clear();
            k.hub.tick();
            assert!(link(&k, 0, outsider) && !link(&k, 1, 2), "{way}: {outsider} with carl again");
        }
    }
}

// the wire: roam_core's advert is the hub's lobby, and the hub's LOBBIES are roam_core's lobbies
#[test]
fn lobbies_on_the_wire_roam_core_and_the_hub_agree() {
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
    if Command::new("node").arg("--version").output().is_err() {
        eprintln!("no node: skipped");
        return;
    }
    let w = Path::new(env!("CARGO_TARGET_TMPDIR")).join("roam_hubwire_lobby.wasm");
    let out = Command::new(wasi.join("bin/clang"))
        .current_dir(repo())
        .args(["--target=wasm32-wasip1-threads", "-pthread", "-O2", "-ffp-contract=off", "-Iruntime", "-o"])
        .arg(&w)
        .args(["web/test/roam_hubwire.c", "runtime/roam_core.c"])
        .arg("-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=268435456")
        .output()
        .expect("clang");
    assert!(out.status.success(), "clang: {}", String::from_utf8_lossy(&out.stderr));
    let run = |args: &[String]| -> String {
        let o = Command::new("node").arg(repo().join("web/test/wasi-run.mjs")).arg(&w).args(args).output().expect("node");
        String::from_utf8_lossy(&o.stdout).trim().to_string()
    };
    let unhex = |s: &str| (0..s.len() / 2).map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap()).collect::<Vec<u8>>();
    let hex = |b: &[u8]| b.iter().map(|x| format!("{x:02x}")).collect::<String>();
    // the C core's advert, from a player (its HELLO with the flag), and another player's world map: the lobby
    let mut k = Kit::new(RoamOptions { degree: 1, ..RoamOptions::default() });
    let h = hello_with(0x1234ABCD, &HelloOpts { core: 5, ..Default::default() });
    k.from(W(0), &with_flags(h, 0x0080));
    join(&mut k, 1, "", true);
    k.from(W(1), &snap(101, [0.0, 0.0, 0.0]));
    let ad = unhex(&run(&["lobby".to_string()]));
    assert_eq!(ad.len(), 16 + 24 + 8 + 4);
    assert_eq!(ad[16 + 23], 25, "the host's away seconds in the byte after n, once reserved: the hub takes the advert all the same");
    k.from(W(0), &ad);
    k.clear();
    k.from(W(1), &mapreq(101, 1));
    assert_eq!(lobbies(&k), [(W(1), 1, vec![(-1234, 567, 0x0BADF00D, 2, 6, 0)])]);
    // the hub's datagrams to a C session that asks (req 1): it holds the lobby
    let frames: Vec<String> = k.sent.lock().iter().map(|(_, f)| hex(&f[16..])).collect();
    let mut args = vec!["lobbies".to_string()];
    args.extend(frames);
    let got = run(&args);
    assert_eq!(got.lines().last(), Some(format!("lobbies 1 1 -1234,567,{},2,6,0", 0x0BADF00Du32).as_str()), "{got}");
}

// a lobby's host is linked with its players and with the players in free roam near its start before any nearer one
// (every pair | the grid): with the start crowded by players who went into its career race (their places count for
// a minute) and 4 links a player, its player 45 m off and one stopped 12 m out are linked with the host, ones in
// free roam 25 or 40 m out are not, nor a player it lists 300 m off (none of a lobby's is: an advert says what it
// likes); once its advert is stale (and the links' hold is over), the nearest again.  A lobby whose start is not
// where its host is gets no links at all.
#[test]
fn a_lobbys_host_is_linked_with_its_players_and_the_ones_at_its_start_first() {
    for (how, o) in ways() {
        // (the players in its race where they are, not away_m up: a crowd that is not in free roam)
        let mut k = Kit::new(RoamOptions { degree: 4, away_m: 1e-9, ..o });
        for i in 0..11 {
            join(&mut k, i, "", true);
        }
        // 0 the host at the start (0, 0); 1 to 4 in a race, 1 m from it all round (its 4 nearest); 5 a player of the
        // lobby 45 m off; 6 stopped 12 m out; 7 at 25 m; 8 at 40 m; 9 a player it lists 300 m off; 10 a host 1 km off
        let snaps = |k: &mut Kit| {
            k.from(W(0), &snap(100, [0.0, 0.0, 0.0]));
            for (i, at) in [[1.0, 0.0], [0.0, 1.0], [-1.0, 0.0], [0.0, -1.0]].iter().enumerate() {
                k.from(W(1 + i as u32), &snap_n(101 + i as u32, [at[0], at[1], 0.0], 3, 1));
            }
            k.from(W(5), &snap(105, [-45.0, 0.0, 0.0]));
            k.from(W(6), &snap(106, [12.0, 0.0, 0.0]));
            k.from(W(7), &snap(107, [25.0, 0.0, 0.0]));
            k.from(W(8), &snap(108, [40.0, 0.0, 0.0]));
            k.from(W(9), &snap(109, [300.0, 0.0, 0.0]));
            k.from(W(10), &snap(110, [1000.0, 0.0, 0.0]));
        };
        let linked = |k: &Kit, a: u32, b: u32| {
            let l = k.lists();
            let has = |x: u32, y: u32| l.get(&format!("{}:47210", W(x))).is_some_and(|v| v.iter().any(|e| e.vip == W(y)));
            assert_eq!(has(a, b), has(b, a), "{how}: a link is listed at both ends");
            has(a, b)
        };
        snaps(&mut k);
        k.hub.tick();
        assert!(!linked(&k, 0, 5) && !linked(&k, 0, 6), "{how}: without a lobby, the nearest");
        // the advert: the host, 5 and 9 in it
        k.from(W(0), &lobby(100, 1, 0, 0xA0, 0x1111, 0, 0, 0, 6, &[100, 105, 109]));
        k.step(1000.0);
        snaps(&mut k);
        k.clear();
        k.hub.tick();
        assert!(linked(&k, 0, 5), "{how}: its player 45 m off");
        assert!(linked(&k, 0, 6), "{how}: the one 12 m out");
        assert!(!linked(&k, 0, 7) && !linked(&k, 0, 8), "{how}: not the ones 25 and 40 m out");
        assert!(!linked(&k, 0, 9), "{how}: not the one it lists 300 m off");
        assert_eq!(k.info("lobbyLinks"), 2.0, "{how}: 2 links for the lobby");
        // 7, 25 m out, asks to join (its JOIN's copy to the hub; one for another lobby counts for nothing): linked
        // with the host too
        k.from(W(7), &lobby(107, 2, 0, 0xA0, 0x1111, 0, 0, 0, 6, &[100, 105]));
        k.from(W(8), &lobby(108, 2, 0, 0xEE, 0x1111, 0, 0, 0, 6, &[100, 105]));
        k.from(W(0), &lobby(100, 1, 0, 0xA0, 0x1111, 0, 0, 0, 6, &[100, 105, 109]));
        k.step(1000.0);
        snaps(&mut k);
        k.clear();
        k.hub.tick();
        assert!(linked(&k, 0, 7), "{how}: the one asking to join, 25 m out");
        assert!(!linked(&k, 0, 8), "{how}: not one asking to join another lobby");
        assert_eq!(k.info("lobbyLinks"), 3.0, "{how}: 3 links for the lobby");
        // no advert since: stale; past the links' hold, the nearest
        for _ in 0..3 {
            k.step(2500.0);
            snaps(&mut k);
            k.clear();
            k.hub.tick();
        }
        assert!(!linked(&k, 0, 5) && !linked(&k, 0, 6) && !linked(&k, 0, 7), "{how}: its advert stale, the nearest again");
        assert_eq!(k.info("lobbyLinks"), 0.0, "{how}: none for a lobby gone");
        // 10, 1 km off, advertises a lobby at the crowded start listing 6: no links for it
        k.from(W(10), &lobby(110, 1, 0, 0xB0, 0x1111, 0, 0, 0, 6, &[110, 106]));
        k.step(1000.0);
        snaps(&mut k);
        k.clear();
        k.hub.tick();
        assert_eq!(k.info("lobbyLinks"), 0.0, "{how}: none for a lobby whose start is not its host's");
    }
}

/// a RACEREP datagram as roam_core.c lays it out: lobby, race; mode, laps, track; place, players, flags, type, class,
/// 3 zero bytes; rating, field, factor, kW, kg, km/h, time; the name in 16 bytes
#[allow(clippy::too_many_arguments)]
fn racerep(sender: u32, lobby: u32, place: u8, players: u8, flags: u8, rating: f32, name: &[u8], payload_len: usize) -> Vec<u8> {
    let mut p = vec![0u8; payload_len];
    p[0..4].copy_from_slice(&lobby.to_le_bytes());
    p[4..8].copy_from_slice(&0x0BADF00Du32.to_le_bytes());
    p[8] = 2; // drag
    p[9] = 1;
    p[10..12].copy_from_slice(&4401u16.to_le_bytes());
    p[12] = place;
    p[13] = players;
    p[14] = flags;
    p[15] = 0;
    p[16] = 3;
    for (k, v) in [rating, 241.4, 1.0, 128.0, 1055.0, 256.0, 0.0].iter().enumerate() {
        p[20 + 4 * k..24 + 4 * k].copy_from_slice(&v.to_le_bytes());
    }
    p[48..48 + name.len()].copy_from_slice(name);
    dgram(14, sender, &p)
}

// a player's race report is one [race] line: the race, how it did, its car; one a race (its lobby) and none sooner
// than 20 s after its last; a report with a field out of range, or from no player the hub knows, is none
#[test]
fn race_reports_are_logged_once_a_race() {
    let mut k = Kit::new(RoamOptions::default());
    k.from(W(0), &hello_with(100, &HelloOpts { core: 5, name: b"Ann", ..Default::default() }));
    k.from(W(1), &racerep(101, 0xA1, 1, 2, 0, 239.3, b"PEUGOT", 64));
    assert!(k.lines.lock().is_empty(), "no report from a browser the hub does not know");
    k.from(W(0), &racerep(100, 0xA1, 0, 2, 2, 239.3, b"PEUGOT", 64));
    assert_eq!(
        k.lines.lock().as_slice(),
        ["[race] lobby 000000a1, race 0badf00d (drag, track 4401, 1 lap, 2 cars): Ann quit; PEUGOT (type 0), class 3, rating 239.3 \
          (the cars' average 241.4), 128 kW, 1055 kg, 256 km/h, power x1.000"]
    );
    k.lines.lock().clear();
    k.step(30000.0);
    k.from(W(0), &racerep(100, 0xA1, 1, 2, 0, 239.3, b"PEUGOT", 64));
    assert!(k.lines.lock().is_empty(), "the same race again: none");
    k.from(W(0), &racerep(100, 0xA2, 1, 2, 0, 239.3, b"PEUGOT", 64));
    assert_eq!(k.lines.lock().len(), 1, "another race: its line");
    assert!(k.lines.lock()[0].contains(": Ann 1st of 2;"), "{:?}", k.lines.lock());
    k.lines.lock().clear();
    k.from(W(0), &racerep(100, 0xA3, 1, 2, 0, 239.3, b"PEUGOT", 64));
    assert!(k.lines.lock().is_empty(), "another race within 20 s of the last: none");
    k.step(25000.0);
    for (what, d) in [
        ("a place past its cars", racerep(100, 0xA4, 3, 2, 0, 239.3, b"PEUGOT", 64)),
        ("no cars", racerep(100, 0xA4, 0, 0, 0, 239.3, b"PEUGOT", 64)),
        ("an unknown flag", racerep(100, 0xA4, 1, 2, 4, 239.3, b"PEUGOT", 64)),
        ("a rating out of range", racerep(100, 0xA4, 1, 2, 0, 9999.0, b"PEUGOT", 64)),
        ("a rating not a number", racerep(100, 0xA4, 1, 2, 0, f32::NAN, b"PEUGOT", 64)),
        ("an unprintable name", racerep(100, 0xA4, 1, 2, 0, 239.3, b"PE\x07GOT", 64)),
        ("a short one", racerep(100, 0xA4, 1, 2, 0, 239.3, b"PEUGOT", 60)),
        ("another sender in the datagram", racerep(101, 0xA4, 1, 2, 0, 239.3, b"PEUGOT", 64)),
    ] {
        k.from(W(0), &d);
        assert!(k.lines.lock().is_empty(), "{what}: none");
    }
    k.from(W(0), &racerep(100, 0xA4, 1, 2, 1, 239.3, b"PEUGOT", 64));
    assert!(k.lines.lock()[0].contains(": Ann (host) 1st of 2;"), "{:?}", k.lines.lock());
    assert_eq!(k.info("raceReports"), 3.0);
}

// the wire: roam_core's report is the hub's line
#[test]
fn race_reports_on_the_wire_roam_core_and_the_hub_agree() {
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
    if Command::new("node").arg("--version").output().is_err() {
        eprintln!("no node: skipped");
        return;
    }
    let w = Path::new(env!("CARGO_TARGET_TMPDIR")).join("roam_hubwire_racerep.wasm");
    let out = Command::new(wasi.join("bin/clang"))
        .current_dir(repo())
        .args(["--target=wasm32-wasip1-threads", "-pthread", "-O2", "-ffp-contract=off", "-Iruntime", "-o"])
        .arg(&w)
        .args(["web/test/roam_hubwire.c", "runtime/roam_core.c"])
        .arg("-Wl,--import-memory,--export-memory,--shared-memory,--initial-memory=67108864,--max-memory=268435456")
        .output()
        .expect("clang");
    assert!(out.status.success(), "clang: {}", String::from_utf8_lossy(&out.stderr));
    let o = Command::new("node").arg(repo().join("web/test/wasi-run.mjs")).arg(&w).arg("racerep").output().expect("node");
    let hexed = String::from_utf8_lossy(&o.stdout).trim().to_string();
    let d: Vec<u8> = (0..hexed.len() / 2).map(|i| u8::from_str_radix(&hexed[2 * i..2 * i + 2], 16).unwrap()).collect();
    assert_eq!(d.len(), 16 + 64 + 4, "{hexed}");
    let mut k = Kit::new(RoamOptions::default());
    k.from(W(0), &hello_with(0x1234ABCD, &HelloOpts { core: 5, name: b"Ann K.", ..Default::default() }));
    k.from(W(0), &d);
    assert_eq!(
        k.lines.lock().as_slice(),
        ["[race] lobby a1b2c3d4, race 0badf00d (sprint, track 4102, 1 lap, 3 cars): Ann K. (host) 2nd of 3 in 95.50 s; 350Z (type 22), \
          class 2, rating 316.6 (the cars' average 280.5), 214 kW, 1450 kg, 249 km/h, power x1.250"]
    );
}
