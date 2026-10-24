//! The Online audit's cases (ONL-1 to ONL-13) at the level of the online host.  The parts that need the LAN switch
//! (ONL-2's "what the host sends for a client is charged to it", the slow receiver) are the integration's.

use std::sync::Arc;

use super::world::{C, Opts, World};
use crate::buddy::BuddyLimits;
use crate::lobby::Limits;
use crate::trace::{LobbyTrace, TraceState};
use crate::vhost::{DATA, lan_frame};
use crate::{PORT_LOBBY, VHOST_VIP};

fn world() -> World {
    World::new(Opts::default())
}

/// a browser of the audit's world: its client is "client <vip>" unless named
fn client(w: &mut World, vip: &str) -> u32 {
    w.client_of(vip, &format!("client {vip}"))
}

/// the game's race report (rank RESU), version 3: players (index, place, time)
fn resu(kind: u8, track: u16, players: &[(u8, u8, f32)]) -> String {
    resu_full(kind, track, &players.iter().map(|&(i, p, t)| (i, p, t, 0, false)).collect::<Vec<_>>())
}

/// the same with each player's drift score and whether it dropped out: (index, place, time, score, dropped)
fn resu_full(kind: u8, track: u16, players: &[(u8, u8, f32, i32, bool)]) -> String {
    let mut b = vec![0u8; 8 + 55 * players.len()];
    b[0] = 3;
    b[1] = players.len() as u8;
    b[2] = kind;
    b[4..6].copy_from_slice(&track.to_be_bytes());
    b[7] = 2;
    for (i, (index, place, time, score, dropped)) in players.iter().enumerate() {
        let o = 8 + 55 * i;
        b[o] = *index;
        b[o + 1] = *place;
        b[o + 2] = if *dropped { 0x0c } else { 1 };
        b[o + 8..o + 12].copy_from_slice(&time.to_be_bytes());
        b[o + 44..o + 48].copy_from_slice(&score.to_be_bytes());
    }
    use base64::Engine;
    base64::engine::general_purpose::STANDARD.encode(b)
}

/// a started race of game `name` between sessions (the first creates it); its AUTH
fn race(w: &mut World, name: &str, sessions: &[C], extra: crate::aries::Fields) -> Option<String> {
    let mut f = fields![("NAME", name), ("PARAMS", "TRACK=4014")];
    f.extend(extra);
    w.send(sessions[0], "gcre", f);
    for s in &sessions[1..] {
        w.send(*s, "gjoi", fields![("NAME", name)]);
    }
    w.send(sessions[0], "gsta", fields![("NAME", name)]);
    w.c(sessions[0]).last("+ses").and_then(|m| m.get("AUTH")).map(str::to_string)
}

fn report(w: &mut World, c: C, auth: &str, names: &[&str], players: &[(u8, u8, f32)], kind: u8) {
    let mut f = fields![("AUTH", auth), ("RESU", resu(kind, 4014, players))];
    for (i, n) in names.iter().enumerate() {
        f.extend(fields![(format!("NAME{i}"), *n)]);
    }
    w.send(c, "rank", f);
}

fn login(w: &mut World, vip: u32, name: &str) -> C {
    w.login(vip, name)
}

// ONL-1: EA Messenger: a message rate, the lists and the search rated, presence only when it changed
#[test]
fn onl1_messenger_rates() {
    let mut w = world();
    let (ga, gb) = (client(&mut w, "10.77.0.2"), client(&mut w, "10.77.0.3"));
    let la = login(&mut w, ga, "Flooder");
    let lb = login(&mut w, gb, "Friend1");
    let (ka, kb) = (w.lkey(la), w.lkey(lb));
    let a = w.buddy(ga, &ka);
    let b = w.buddy(gb, &kb);
    assert_eq!(w.c(a).l("AUTH").code, "");
    w.send(b, "RADD", fields![("USER", "Flooder"), ("LIST", "B"), ("ID", 1)]);
    let pgets = |w: &World| w.c(b).msgs.iter().filter(|m| m.kind == "PGET" && m.get("USER") == Some("Flooder")).count();
    // ten presences that change nothing, then ten that do, in one moment: the friend hears of the first and, a
    // second later, of the last
    let p0 = pgets(&w);
    for _ in 0..10 {
        w.send(a, "PSET", fields![("SHOW", "CHAT"), ("STAT", ""), ("PROD", "same")]);
    }
    let p1 = pgets(&w);
    assert!(p1 - p0 <= 1, "the same presence ten times: {} PGET to the friend", p1 - p0);
    for i in 0..10 {
        w.send(a, "PSET", fields![("SHOW", "CHAT"), ("STAT", ""), ("PROD", format!("p{i}"))]);
    }
    assert!(pgets(&w) - p1 <= 1, "ten changes in one moment: {} PGET at once", pgets(&w) - p1);
    w.sleep(1200.0);
    let last = w.c(b).msgs.iter().rev().find(|m| m.kind == "PGET" && m.get("USER") == Some("Flooder")).unwrap();
    assert_eq!(last.g("PROD"), "p9", "the latest presence follows");
    // the lists: four at once, then "full"; a flood of messages closes the connection
    w.advance(10_000.0);
    for i in 0..100 {
        w.send(a, "RGET", fields![("LIST", "B"), ("ID", i)]);
    }
    let answered = w.c(a).msgs.iter().filter(|m| m.kind == "RGET" && m.code.is_empty()).count();
    assert!(answered <= 5, "RGET answered with the list {answered} times out of 100");
    assert!(w.c(a).reset, "a flood of messages: the connection is closed");
    // the search: four at once, then nothing
    let l2 = login(&mut w, gb, "Friend2");
    let k2 = w.lkey(l2);
    let c = w.buddy(gb, &k2);
    for i in 0..10 {
        w.send(c, "USCH", fields![("USER", "flo"), ("ID", i)]);
    }
    let found = w.c(c).msgs.iter().filter(|m| m.kind == "USCH" && m.get("SIZE") != Some("0")).count();
    assert!(found <= 4, "10 searches: {found} done");
}

// ONL-2: the lobby: game records to the game list only, at most once a second a game; populations the same; a
// gset that changes nothing goes nowhere
#[test]
fn onl2_lobby_announcements() {
    let mut w = world();
    let g = client(&mut w, "10.77.0.2");
    let host = login(&mut w, g, "MalHost");
    let g = client(&mut w, "10.77.0.3");
    let mate = login(&mut w, g, "Mate");
    let viewers: Vec<C> = [4, 5, 6]
        .iter()
        .map(|i| {
            let g = client(&mut w, &format!("10.77.0.{i}"));
            login(&mut w, g, &format!("View{i}"))
        })
        .collect();
    let idle: Vec<C> = [7, 8]
        .iter()
        .map(|i| {
            let g = client(&mut w, &format!("10.77.0.{i}"));
            login(&mut w, g, &format!("Idle{i}"))
        })
        .collect();
    for v in &viewers {
        w.send(*v, "gsea", fields![("START", 0), ("COUNT", 20)]);
    }
    w.send(host, "gcre", fields![("NAME", "junk"), ("PARAMS", "TRACK=1")]);
    w.send(mate, "gjoi", fields![("NAME", "junk")]);
    for i in 0..30 {
        w.send(host, "gset", fields![("PARAMS", format!("TRACK={i}"))]);
    }
    for v in &viewers {
        assert!(w.c(*v).count("+agm") <= 3, "a viewer got {} game records for 31 changes in one moment", w.c(*v).count("+agm"));
    }
    for v in &idle {
        assert_eq!(w.c(*v).count("+agm"), 0, "a player not looking at the game list gets no game records");
    }
    w.advance(1000.0);
    for v in &viewers {
        assert_eq!(w.c(*v).l("+agm").g("PARAMS"), "TRACK=29", "the latest record follows");
    }
    // a gset that changes nothing: nothing to the game's players, nothing to the list
    let (m0, a0) = (w.c(mate).count("+mgm"), w.c(viewers[0]).count("+agm"));
    for _ in 0..10 {
        w.send(host, "gset", fields![("PARAMS", "TRACK=29")]);
    }
    w.advance(1000.0);
    assert_eq!(w.c(mate).count("+mgm") - m0, 0, "unchanged settings: no update to the players");
    assert_eq!(w.c(viewers[0]).count("+agm") - a0, 0, "unchanged settings: no update to the list");
    // rooms entered and left over and over: the others hear of a room's population at most once a second
    let pop0 = w.c(viewers[1]).count("+pop");
    for i in 0..20 {
        w.send(idle[0], "move", fields![("NAME", if i % 2 == 1 { "Lounge" } else { "Circuit" })]);
    }
    let n = w.c(viewers[1]).count("+pop") - pop0;
    assert!(n <= 4, "20 moves: {n} population updates to another player");
}

// (ONL-2's second half at the host's level: a receiver the switch takes no more frames for loses its online stream)
#[test]
fn onl2_slow_receiver_loses_its_stream() {
    let mut w = world();
    let (ga, gb) = (client(&mut w, "10.77.0.2"), client(&mut w, "10.77.0.3"));
    let a = login(&mut w, ga, "Victim0");
    let b = login(&mut w, gb, "Victim1");
    w.send(a, "move", fields![("NAME", "Lounge")]);
    w.send(b, "move", fields![("NAME", "Lounge")]);
    w.slow.lock().insert(ga);
    w.send(b, "mesg", fields![("TEXT", "hello room")]);
    assert!(!w.personas().contains(&Some("Victim0".into())), "its lobby session went");
    assert_eq!(w.core.info()["slow"], 1);
    assert!(!w.c(b).reset);
}

// ONL-3: EA Messenger memory: a friend taken off is gone, only adding a friend makes a list, lists are capped, a
// second login is refused
#[test]
fn onl3_messenger_memory() {
    let mut w = World::new(Opts { buddy: BuddyLimits { lists: 10.0, list_rate: 5.0, ..Default::default() }, ..Default::default() });
    let g = client(&mut w, "10.77.0.2");
    let l = login(&mut w, g, "Flooder");
    let k = w.lkey(l);
    let b = w.buddy(g, &k);
    for i in 0..300 {
        w.send(b, "RADD", fields![("USER", format!("Ghost{i}")), ("LIST", "B")]);
        w.send(b, "RDEL", fields![("USER", format!("Ghost{i}")), ("LIST", "B")]);
        if i % 10 == 9 {
            w.advance(3000.0);
        }
    }
    assert!(!w.c(b).reset);
    assert_eq!(w.core.buddies.list("Flooder").unwrap().b.len(), 0, "names of friends taken off are gone");
    // a persona that only asks and talks has no list
    let g2 = client(&mut w, "10.77.0.3");
    let l2 = login(&mut w, g2, "Quiet");
    let k2 = w.lkey(l2);
    let q = w.buddy(g2, &k2);
    w.send(q, "EPGT", fields![("ID", 1)]);
    w.send(q, "RGET", fields![("LIST", "B")]);
    w.send(q, "PSET", fields![("SHOW", "CHAT")]);
    w.send(q, "PING", vec![]);
    assert!(!w.core.buddies.lists.contains_key("quiet"), "no list without a friend added");
    // a second login on a connection that is logged in
    let l3 = login(&mut w, g2, "Quiet2");
    let k3 = w.lkey(l3);
    w.send(q, "AUTH", fields![("LKEY", k3)]);
    assert_eq!(w.c(q).l("AUTH").code, "logn");
    // one client makes list_rate new lists an hour; the store keeps `lists` of them, the least recently seen going
    let made = |w: &mut World, group: &str| {
        let mut ok = 0;
        for i in 0..6 {
            let c = w.client_of(&format!("10.77.{}.{}", group.as_bytes()[0], 10 + i), group);
            let cl = login(w, c, &format!("P{group}{i}"));
            let k = w.lkey(cl);
            let cb = w.buddy(c, &k);
            w.send(cb, "RADD", fields![("USER", "Someone"), ("LIST", "B"), ("ID", 1)]);
            if w.c(cb).l("RADD").code.is_empty() {
                ok += 1;
            }
        }
        ok
    };
    assert!(made(&mut w, "G") <= 5, "one client: at most listRate new lists");
    made(&mut w, "H");
    made(&mut w, "I");
    assert!(w.core.buddies.lists.len() <= 10, "{} lists kept (the cap is 10)", w.core.buddies.lists.len());
}

// ONL-4: a Messenger connection that logged in twice and closed breaks nobody's Messenger
#[test]
fn onl4_double_login() {
    let mut w = world();
    let v = client(&mut w, "10.77.0.10");
    let l = login(&mut w, v, "Victim1");
    let k = w.lkey(l);
    let vb = w.buddy(v, &k);
    w.send(vb, "PSET", fields![("SHOW", "CHAT"), ("STAT", "EX=0"), ("PROD", "x")]);
    let a = client(&mut w, "10.77.0.20");
    let l1 = login(&mut w, a, "Mal1");
    let l2 = login(&mut w, a, "Mal2");
    let (k1, k2) = (w.lkey(l1), w.lkey(l2));
    let ab = w.buddy(a, &k1);
    w.send(ab, "AUTH", fields![("LKEY", k2)]);
    w.close(ab);
    w.send(vb, "PSET", fields![("SHOW", "AWAY"), ("STAT", "EX=0"), ("PROD", "x")]);
    assert!(!w.c(vb).reset, "the victim's Messenger after the other left");
    let n = client(&mut w, "10.77.0.11");
    let ln = login(&mut w, n, "Victim2");
    let kn = w.lkey(ln);
    let nb = w.buddy(n, &kn);
    w.send(nb, "PSET", fields![("SHOW", "CHAT")]);
    assert!(!w.c(nb).reset, "a new player's Messenger");
    let x = client(&mut w, "10.77.0.12");
    let lx = login(&mut w, x, "Victim3");
    let kx = w.lkey(lx);
    let xb = w.buddy(x, &kx);
    w.send(xb, "SEND", fields![("USER", "Mal1"), ("BODY", "hi")]);
    w.send(xb, "SEND", fields![("USER", "Mal2"), ("BODY", "hi")]);
    assert!(!w.c(xb).reset, "a message to the names that connection used");
    assert!(w.core.errors.is_empty(), "no handler failed: {:?}", w.core.errors);
}

// ONL-5: one browser is one player of a race; a browser keeps few open games; a race whose players left the server
// ends
#[test]
fn onl5_races_and_browsers() {
    let mut w = world();
    let one = client(&mut w, "10.77.0.2");
    let sa = login(&mut w, one, "MalA");
    let sb = login(&mut w, one, "MalB");
    w.send(sa, "gcre", fields![("NAME", "g1")]);
    w.send(sb, "gjoi", fields![("NAME", "g1")]);
    w.send(sa, "gsta", fields![("NAME", "g1")]);
    assert_eq!(w.c(sa).l("gsta").code, "nepl", "two players in one browser cannot start a race");
    // open games: openPerVip (2) for one browser
    let b = client(&mut w, "10.77.0.3");
    let s1 = login(&mut w, b, "Open1");
    let s2 = login(&mut w, b, "Open2");
    let s3 = login(&mut w, b, "Open3");
    w.send(s1, "gcre", fields![("NAME", "o1")]);
    w.send(s2, "gcre", fields![("NAME", "o2")]);
    w.send(s3, "gcre", fields![("NAME", "o3")]);
    assert_eq!(w.c(s3).l("gcre").code, "full", "a third open game from one browser");
    // a race whose players' browsers left the switch ends after a while; one whose players are still here goes on
    let ps: Vec<C> = (4..=7)
        .map(|i| {
            let g = client(&mut w, &format!("10.77.0.{i}"));
            login(&mut w, g, &format!("Racer{}", i - 3))
        })
        .collect();
    assert!(race(&mut w, "left", &ps[0..2], vec![]).is_some());
    assert!(race(&mut w, "stays", &ps[2..4], vec![]).is_some());
    for c in &ps {
        w.close(*c);
    }
    w.peer_gone(super::world::ip("10.77.0.4"));
    w.peer_gone(super::world::ip("10.77.0.5"));
    w.advance(130_000.0);
    let names: Vec<String> = w.core.lobby.games.values().map(|g| g.name.clone()).collect();
    assert!(!names.contains(&"left".to_string()), "the race of browsers that left ended");
    assert!(names.contains(&"stays".to_string()), "the race of browsers still here goes on");
}

// ONL-6, as the players chose on 2026-10-23 (it was: the blocked list is the session's own, never stored, so that the
// next holder of the name can neither read nor empty it): the blocked list follows the name like its friends, kept
// over a restart, and the block holds while its persona is away (no message waits for it, no request arrives)
#[test]
fn onl6_blocked_list_follows_the_name() {
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("onl6");
    let mut w = World::new(Opts { state_dir: Some(dir.clone()), ..Default::default() });
    let ga = client(&mut w, "10.77.0.50");
    let la = w.login_as(ga, "AliceAcct", "Alice", "secret");
    let ka = w.lkey(la);
    let ab = w.buddy(ga, &ka);
    for (l, u) in [("B", "BobFriend"), ("B", "Mallory"), ("I", "Mallory")] {
        w.send(ab, "RADD", fields![("LIST", l), ("USER", u), ("ID", 1)]);
    }
    // while Alice is online the block works
    let gm = client(&mut w, "10.77.0.51");
    let lm = login(&mut w, gm, "Mallory");
    let km = w.lkey(lm);
    let mb = w.buddy(gm, &km);
    w.send(mb, "SEND", fields![("USER", "Alice"), ("BODY", "hi")]);
    assert_eq!(w.c(ab).count("RECV"), 0, "a blocked sender's message does not arrive");
    // and while she is away, though Mallory is on her friends list too
    w.close(ab);
    w.close(la);
    w.send(mb, "SEND", fields![("USER", "Alice"), ("BODY", "still there?")]);
    w.send(mb, "RADM", fields![("USER", "Alice"), ("ID", 3)]);
    w.close(mb);
    w.core.buddy_save();
    let stored = std::fs::read_to_string(dir.join("buddies.json")).unwrap();
    assert!(stored.contains(r#""I":["Mallory"]"#), "the blocked list is kept: {stored}");
    assert!(!stored.contains("still there") && !stored.contains(r#""R""#), "nothing of Mallory's waits: {stored}");
    // over a restart: whoever plays as Alice has the list
    w.core.stop();
    let mut w = World::new(Opts { state_dir: Some(dir.clone()), ..Default::default() });
    let g2 = client(&mut w, "10.77.0.52");
    let l2 = w.login_as(g2, "Other", "Alice", "secret");
    let k2 = w.lkey(l2);
    let b2 = w.buddy(g2, &k2);
    w.send(b2, "RGET", fields![("LIST", "I"), ("PRES", "Y"), ("PEND", "Y"), ("ID", 2)]);
    assert_eq!([w.c(b2).l("RGET").g("SIZE"), w.c(b2).l("ROST").g("USER"), w.c(b2).l("ROST").g("ID")], ["1", "Mallory", "2"]);
    // unblocked: gone from the file
    w.send(b2, "RDEL", fields![("LIST", "I"), ("USER", "Mallory")]);
    w.core.buddy_save();
    assert!(!std::fs::read_to_string(dir.join("buddies.json")).unwrap().contains(r#""I""#));
}

fn board(w: &mut World, kind: u32) -> Vec<String> {
    let mut b: Vec<String> =
        w.core.stats.board(kind, false).iter().map(|r| format!("{}:{}:{}:{}", r.name, r.points, r.wins, r.losses)).collect();
    b.sort();
    b
}

// ONL-7: results: one report or reports that differ decide nothing; ranked needs three; a race too short is not
// taken
#[test]
fn onl7_results() {
    let mut w = world();
    // (a) one player's report alone
    let g = client(&mut w, "10.77.0.2");
    let m = login(&mut w, g, "Mallory");
    let g = client(&mut w, "10.77.0.3");
    let al = login(&mut w, g, "Alice");
    let auth = race(&mut w, "a", &[m, al], vec![]).unwrap();
    w.advance(60_000.0);
    report(&mut w, m, &auth, &["Mallory", "Alice"], &[(0, 1, 50.0), (1, 2, 55.0)], 0);
    assert!(board(&mut w, 0).is_empty(), "one report decides nothing");
    // (b) two reports that differ: neither counts
    let g = client(&mut w, "10.77.0.4");
    let bob = login(&mut w, g, "Bob");
    let g = client(&mut w, "10.77.0.5");
    let carol = login(&mut w, g, "Carol");
    let auth = race(&mut w, "b", &[bob, carol], vec![]).unwrap();
    w.advance(60_000.0);
    report(&mut w, carol, &auth, &["Bob", "Carol"], &[(1, 1, 50.0), (0, 2, 51.0)], 0);
    report(&mut w, bob, &auth, &["Bob", "Carol"], &[(0, 1, 50.0), (1, 2, 51.0)], 0);
    assert!(board(&mut w, 0).is_empty(), "reports that differ decide nothing");
    // (c) a ranked race of two: counted, no points
    let g = client(&mut w, "10.77.0.6");
    let x = login(&mut w, g, "Xeno");
    let g = client(&mut w, "10.77.0.7");
    let y = login(&mut w, g, "Yuri");
    let auth = race(&mut w, "c", &[x, y], fields![("SYSFLAGS", 1 << 18)]).unwrap();
    w.advance(60_000.0);
    for c in [x, y] {
        report(&mut w, c, &auth, &["Xeno", "Yuri"], &[(0, 1, 50.0), (1, 2, 51.0)], 0);
    }
    assert_eq!(board(&mut w, 0), ["Xeno:0:1:0", "Yuri:0:0:1"], "a ranked race of two players gives no points");
    // (d) too short for its type, though both agree
    let g = client(&mut w, "10.77.0.8");
    let p = login(&mut w, g, "Quick1");
    let g = client(&mut w, "10.77.0.9");
    let q = login(&mut w, g, "Quick2");
    let auth = race(&mut w, "d", &[p, q], vec![]).unwrap();
    w.advance(3000.0);
    for c in [p, q] {
        report(&mut w, c, &auth, &["Quick1", "Quick2"], &[(0, 1, 1.5), (1, 2, 2.0)], 0);
    }
    assert!(!board(&mut w, 0).iter().any(|r| r.starts_with("Quick")), "a 3-second circuit race is not taken");
}

// results: race types by the game's numbers (drag 2, drift 3); its seventh (6) is a result that counts nowhere
#[test]
fn race_types() {
    let mut w = world();
    let boards = |w: &mut World| (0..6).map(|t| w.core.stats.board(t, false).len()).collect::<Vec<_>>();
    // a quarter-mile drag: times well under the other types' 20 s
    let g = client(&mut w, "10.77.0.2");
    let a = login(&mut w, g, "Drag1");
    let g = client(&mut w, "10.77.0.3");
    let b = login(&mut w, g, "Drag2");
    let auth = race(&mut w, "drag", &[a, b], vec![]).unwrap();
    w.advance(40_000.0);
    for c in [a, b] {
        report(&mut w, c, &auth, &["Drag1", "Drag2"], &[(0, 1, 11.2), (1, 2, 12.5)], 2);
    }
    assert!(w.text().iter().any(|l| l.ends_with("\"drag\" (drag, unranked): 1 Drag1 0:11.20, 2 Drag2 0:12.50")), "{}", w.text().join("\n"));
    // drift: places by points, not by time
    let g = client(&mut w, "10.77.0.4");
    let c = login(&mut w, g, "Drift1");
    let g = client(&mut w, "10.77.0.5");
    let d = login(&mut w, g, "Drift2");
    let auth = race(&mut w, "drift", &[c, d], vec![]).unwrap();
    w.advance(110_000.0);
    for x in [c, d] {
        report(&mut w, x, &auth, &["Drift1", "Drift2"], &[(0, 1, 104.0), (1, 2, 92.0)], 3);
    }
    assert!(w.logged("\"drift\" (drift, unranked): 1 Drift1"), "{}", w.text().join("\n"));
    assert_eq!(boards(&mut w), [0, 0, 2, 2, 0, 0]);
    // the seventh: the result is taken and logged; no board or career counts it
    let g = client(&mut w, "10.77.0.6");
    let e = login(&mut w, g, "Seven1");
    let g = client(&mut w, "10.77.0.7");
    let f = login(&mut w, g, "Seven2");
    let auth = race(&mut w, "seven", &[e, f], vec![]).unwrap();
    w.advance(110_000.0);
    for x in [e, f] {
        report(&mut w, x, &auth, &["Seven1", "Seven2"], &[(0, 1, 100.0), (1, 2, 104.0)], 6);
    }
    assert!(
        w.text().iter().any(|l| l.contains("\"seven\" (type 6, unranked): 1 Seven1") && l.ends_with("(no stats for this race type)")),
        "{}",
        w.text().join("\n")
    );
    assert!(!w.logged("not taken"), "{}", w.text().join("\n"));
    assert_eq!(boards(&mut w), [0, 0, 2, 2, 0, 0]);
    assert_eq!(w.core.lobby.games.len(), 0, "every race is over");
}

// a ranked race's table on its players' results screens: whose reports came ($rup) until the result stands, then
// places, times or drift scores and points in hex ($rfn) to every reporter, and to a later one; unranked: none; the
// careers of the players who reported before it stood are sent again, with the race's points (issue #12)
#[test]
fn ranked_table() {
    let mut w = world();
    let ranked = fields![("SYSFLAGS", 1 << 18)];
    let names = ["Ann", "Bob", "Cat"];
    let gs: Vec<u32> = (0..3).map(|i| client(&mut w, &format!("10.77.0.{}", i + 2))).collect();
    // logged in (again: after a race the game's results screen logs in and reports)
    let login_all = |w: &mut World| -> Vec<C> { (0..3).map(|i| login(w, gs[i], names[i])).collect() };
    let rep = |w: &mut World, c: C, auth: &str, kind: u8, players: &[(u8, u8, f32, i32, bool)]| {
        let f = fields![("AUTH", auth), ("RESU", resu_full(kind, 4014, players)), ("NAME0", "Ann"), ("NAME1", "Bob"), ("NAME2", "Cat")];
        w.send(c, "rank", f);
    };
    let row = |m: &super::world::M, i: usize| format!("{} {}", m.g(&format!("OPPO{i}")), m.g(&format!("DATA{i}")));
    // a circuit: Bob first in 186.5 s, Ann second in 190.25 s, Cat dropped out after 60 s
    let s = login_all(&mut w);
    let auth = race(&mut w, "c", &s, ranked.clone()).unwrap();
    w.advance(200_000.0);
    let s = login_all(&mut w);
    let circuit = [(0, 2, 190.25, 0, false), (1, 1, 186.5, 0, false), (2, 3, 60.0, 0, true)];
    rep(&mut w, s[1], &auth, 0, &circuit);
    let up = w.c(s[1]).l("$rup");
    let stat: Vec<String> = (0..3).map(|i| format!("{} {}", up.g(&format!("OPPO{i}")), up.g(&format!("STAT{i}")))).collect();
    assert_eq!(stat, ["Ann 0", "Bob 1", "Cat 0"]);
    assert_eq!(up.g("TIME"), "60000");
    assert_eq!(w.c(s[1]).count("$rfn"), 0, "one report decides nothing");
    rep(&mut w, s[0], &auth, 0, &circuit);
    for c in [s[0], s[1]] {
        let t = w.c(c).l("$rfn");
        assert_eq!((0..3).map(|i| row(t, i)).collect::<Vec<_>>(), ["Ann 2e72a,2,0,0,32", "Bob 2d884,1,0,0,64", "Cat ea60,-2,0,0,0"]);
    }
    assert_eq!(w.c(s[0]).count("$rup"), 0, "the result stood with Ann's report");
    assert_eq!(board(&mut w, 0), ["Ann:50:0:1", "Bob:100:1:0", "Cat:0:0:1"], "the table's points are the rankings'");
    // and the careers': Bob's report came before the result stood, and his game is told his 100 points too (STAT's
    // circuit rating, its 9th number)
    let rating = |w: &World, c: C| w.c(c).l("user").g("STAT").split(',').nth(8).map(str::to_string);
    assert_eq!((rating(&w, s[1]), rating(&w, s[0])), (Some("64".into()), Some("32".into())));
    // a later report: the table to it alone
    rep(&mut w, s[2], &auth, 0, &circuit);
    assert_eq!(row(w.c(s[2]).l("$rfn"), 2), "Cat ea60,-2,0,0,0");
    assert_eq!((w.c(s[0]).count("$rfn"), w.c(s[1]).count("$rfn")), (1, 1));
    // drift: the score in place of the time
    let auth = race(&mut w, "d", &s, ranked).unwrap();
    w.advance(200_000.0);
    let s = login_all(&mut w);
    let drift = [(0, 1, 150.0, 41_250, false), (1, 3, 150.0, 9_800, false), (2, 2, 150.0, 22_001, false)];
    for c in [s[2], s[0]] {
        rep(&mut w, c, &auth, 3, &drift);
    }
    let t = w.c(s[0]).l("$rfn");
    assert_eq!((0..3).map(|i| row(t, i)).collect::<Vec<_>>(), ["Ann a122,1,0,0,64", "Bob 2648,3,0,0,0", "Cat 55f1,2,0,0,32"]);
    // unranked: the game's own results screen, no table
    let auth = race(&mut w, "u", &s, vec![]).unwrap();
    w.advance(200_000.0);
    let s = login_all(&mut w);
    for c in &s {
        rep(&mut w, *c, &auth, 0, &circuit);
    }
    assert!(w.logged("\"u\" (circuit, unranked)"), "{}", w.text().join("\n"));
    assert_eq!(s.iter().map(|c| w.c(*c).count("$rup") + w.c(*c).count("$rfn")).sum::<usize>(), 0);
}

// ONL-7: the results store: the rankings built once for many asks (the store's other parts: stats.rs)
#[test]
fn onl7_rankings_built_once() {
    let mut w = world();
    let g = client(&mut w, "10.77.0.2");
    let a = login(&mut w, g, "Asker");
    let before = w.core.stats.builds;
    for i in 0..30 {
        w.send(a, "snap", fields![("INDEX", i), ("CHAN", 6), ("START", 0), ("RANGE", 10)]);
    }
    let n = w.core.stats.builds - before;
    assert!(n <= 2, "the board was built {n} times for 30 asks");
}

// ONL-8: no line per friend added; the lines a client causes carry its key
#[test]
fn onl8_lines_and_keys() {
    let mut w = world();
    let g = w.client_of("10.77.0.2", "G8");
    let l = login(&mut w, g, "Flooder");
    let k = w.lkey(l);
    let b = w.buddy(g, &k);
    for i in 0..20 {
        w.send(b, "RADD", fields![("USER", format!("Pal{i}")), ("LIST", "B")]);
    }
    assert!(!w.logged("added a friend"));
    let key = w.lines.borrow().iter().find(|(s, _)| s.contains("logged in as \"Flooder\"")).and_then(|(_, k)| k.clone());
    assert_eq!(key.as_deref(), Some("G8"), "a client's line carries its key (logged a few a minute per key)");
}

// ONL-9: one client holds only its share of the lobby; the last quarter is for clients that hold none
#[test]
fn onl9_share_of_the_lobby() {
    let mut w = World::new(Opts { limits: Limits { sessions: 12.0, ..Default::default() }, ..Default::default() });
    let mut conns = Vec::new();
    for i in 0..3 {
        for _ in 0..3 {
            let g = w.client_of(&format!("10.77.0.{}", 10 + i), "Big");
            conns.push(w.connect(g, PORT_LOBBY));
        }
    }
    assert_eq!(conns.iter().filter(|c| w.c(**c).reset).count(), 1, "one client: 8 lobby sessions, the 9th refused");
    // 8 + 1 of 12 in use: the last quarter only for a client with none
    let g = w.client_of("10.77.0.20", "Other");
    let other = w.connect(g, PORT_LOBBY);
    assert!(!w.c(other).reset);
    let g = w.client_of("10.77.0.21", "Other");
    let again = w.connect(g, PORT_LOBBY);
    assert!(w.c(again).reset, "a client that holds a session already: none of the last quarter");
    let g = w.client_of("10.77.0.22", "Fresh");
    let fresh = w.connect(g, PORT_LOBBY);
    assert!(!w.c(fresh).reset, "a client that holds none gets one");
}

// ONL-12: the trace file is 0600, shows no LKEY, and follows a bounded number of streams
#[test]
fn onl12_trace() {
    let ip = super::world::ip;
    let key = "abcdef0123456789abcdef01234567";
    let first = lan_frame(
        4,
        VHOST_VIP,
        ip("10.77.0.2"),
        PORT_LOBBY,
        40000,
        DATA,
        7,
        &crate::aries::encode_frame("pers", "", &fields![("PERS", "Alpha"), ("LKEY", key)]).unwrap(),
    );
    let partial = |i: u32| lan_frame(4, ip("10.77.0.3"), VHOST_VIP, 41000 + i as u16, PORT_LOBBY, DATA, i, b"partial");
    // the streams it follows, bounded
    let mut st = TraceState::new(Vec::new(), 0);
    st.frame(0, &first);
    for i in 0..300 {
        st.frame(0, &partial(i));
    }
    assert!(st.streams.len() <= 256, "{} streams followed", st.streams.len());
    // the file
    let tmp = tempfile::tempdir().unwrap();
    let file = tmp.path().join("trace.txt");
    let tr = LobbyTrace::open(&file).unwrap();
    let tap = tr.tap();
    tap(&first);
    for i in 0..300 {
        tap(&partial(i));
    }
    tr.close();
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(std::fs::metadata(&file).unwrap().permissions().mode() & 0o777, 0o600);
    }
    let text = std::fs::read_to_string(&file).unwrap();
    assert!(text.contains("PERS=\"Alpha\"") && !text.contains(key), "{text}");
    let _: Arc<LobbyTrace> = tr;
}

// ONL-13: the online folder is 0700; a Messenger login key works only from its own browser
#[test]
fn onl13_folder_and_key() {
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("onl13").join("online");
    let mut w = World::new(Opts { state_dir: Some(dir.clone()), ..Default::default() });
    let g = client(&mut w, "10.77.0.2");
    let l = login(&mut w, g, "Alpha");
    let k = w.lkey(l);
    let b = w.buddy(g, &k);
    w.send(b, "RADD", fields![("USER", "Bravo"), ("LIST", "B")]);
    w.core.buddy_save();
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(std::fs::metadata(&dir).unwrap().permissions().mode() & 0o777, 0o700);
    }
    let o = client(&mut w, "10.77.0.9");
    let other = w.buddy(o, &k);
    w.sleep(3500.0);
    assert_eq!(w.c(other).last("AUTH").map(|m| m.code.as_str()), Some("pass"), "Alpha's key from another browser");
    w.core.stop();
}
