//! The online host's services, driven by fake game clients that speak the switch's frames, as the browsers' runtime
//! does.  The redirector (@dir to the lobby; no answer to @tic), the login (any name; the reply within the game's 122
//! bytes; PASS, REGKEY and PSES never kept), personas (one holder per name, the LKEY the buddy login gets), the config
//! blob, rooms and chat (with the flood mute), the game list, a game from creation to the race start (+ses with every
//! player's virtual address, the same AUTH for all), the countdown of a start before every player is ready and the kick
//! of those still not ready, the lobby link closing at the start without ending the race, game passwords, leaving and
//! deleting, the buddy login, and the limits.

use super::world::{C, Opts, World, ip};
use crate::aries::{Fields, V, encode_fields, encode_frame};
use crate::lobby::Limits;
use crate::stats::{Race, RacePlayer};
use crate::vhost::{DATA, lan_frame};
use crate::{PORT_BUDDY, PORT_HTTP, PORT_LOBBY, PORT_REDIRECTOR, VHOST_VIP};

fn world() -> World {
    World::new(Opts::default())
}

fn is_hex(s: &str, n: usize) -> bool {
    s.len() == n && s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

/// `/^<prefix>\d{4}$/`
fn digits4(s: &str, prefix: &str) -> bool {
    s.strip_prefix(prefix).is_some_and(|d| d.len() == 4 && d.bytes().all(|b| b.is_ascii_digit()))
}

fn fields_len(m: &super::world::M) -> usize {
    let f: Fields = m.f.0.iter().map(|(k, v)| (k.clone().into(), V::from(v))).collect();
    encode_fields(&f, b'\n').unwrap().len()
}

// redirector: @dir points at the lobby, @tic gets no answer, then the link closes
#[test]
fn redirector() {
    let mut w = world();
    let g = w.client("10.77.0.2");
    let c = w.connect(g, PORT_REDIRECTOR);
    assert!(w.c(c).open);
    w.send_text(c, "@tic", "RC4+MD5-V2");
    assert_eq!(w.c(c).msgs.len(), 0);
    w.send(c, "@dir", fields![("PROD", "nfs-pc-2005"), ("MID", "$024e564c0002")]);
    let d = w.c(c).l("@dir");
    assert_eq!(d.g("ADDR"), "10.77.0.1");
    assert_eq!(d.g("PORT"), PORT_LOBBY.to_string());
    assert!(is_hex(d.g("MASK"), 32));
    assert!(d.g("SESS").parse::<u64>().unwrap() > 0);
    assert!(w.c(c).fin, "the redirector closes after its answer");
    let ssl = w.connect(g, 20921);
    assert!(w.c(ssl).reset, "no SSL port: refused");
}

// login: config, any name, personas, LKEY, nothing secret kept
#[test]
fn login() {
    let mut w = world();
    let g = w.client("10.77.0.2");
    let c = w.login_as(g, "Speedy", "Speedy", "CANARY-PASSWORD-123");
    let m = w.cm(c).take();
    let kinds: Vec<String> = m.iter().map(|x| format!("{}/{}", x.kind, x.code)).collect();
    assert_eq!(kinds[..4], ["skey/", "news/new7", "sele/", "auth/"]);
    let news = m.iter().find(|x| x.kind == "news").unwrap();
    assert_eq!(news.g("BUDDY_SERVER"), "10.77.0.1");
    assert_eq!(news.g("BUDDY_PORT"), PORT_BUDDY.to_string());
    let auth = m.iter().find(|x| x.kind == "auth").unwrap();
    assert_eq!(auth.g("PERSONAS"), "Speedy");
    assert_eq!(auth.g("ADDR"), "10.77.0.2", "the virtual address, never another");
    assert!(fields_len(auth) <= 122, "the auth reply fits the game's 122 bytes");
    assert_eq!(m.iter().filter(|x| x.kind == "+rom").count(), 7);
    let pers = m.iter().find(|x| x.kind == "pers").unwrap();
    assert_eq!(pers.g("PERS"), "Speedy");
    assert!(is_hex(pers.g("LKEY"), 30));
    assert!(fields_len(pers) <= 108);
    assert!(m.iter().any(|x| x.kind == "+who") && m.iter().any(|x| x.kind == "+sst"));
    // the server keeps nothing of the password, the CD key or the file checksum, and logs none of them
    let s = w.core.lobby.s(*w.core.lobby.sessions.iter().next().unwrap());
    let dump = format!("{:?} {:?} {:?} {:?} {} {} {}", s.account, s.personas, s.persona, s.lkey, s.addr, s.auxi, s.group);
    let lines = w.text().join("\n");
    for secret in ["CANARY-PASSWORD-123", "AAAA-BBBB", "0123456789abcdef", "024e564c0002"] {
        assert!(!dump.contains(secret), "session keeps {secret}");
        assert!(!lines.contains(secret), "log has {secret}");
    }
    // one holder per persona name while online (another browser gets it with digits); a bad name is refused
    let g2 = w.client("10.77.0.3");
    let c2 = w.login(g2, "Speedy");
    assert!(digits4(w.c(c2).l("pers").g("PERS"), "Speedy"));
    let c3 = w.login(g2, "x y");
    assert_eq!(w.c(c3).l("auth").code, "imst");
    // ... and so is one that reads as a slur (nfsu2-names), as an account or a persona
    let c4 = w.login(g2, "F4g_42");
    assert_eq!(w.c(c4).l("auth").code, "imst");
    // a second persona for this session (create, select, delete)
    w.send(c, "cper", fields![("PERS", "Second")]);
    assert_eq!(w.c(c).l("cper").code, "");
    assert_eq!(w.c(c).l("cper").g("PERS"), "Second");
    w.send(c, "cper", fields![("PERS", "Speedy")]);
    assert_eq!(w.c(c).l("cper").code, "dupl");
    w.send(c, "cper", fields![("PERS", "Sp1cs")]);
    assert_eq!(w.c(c).l("cper").code, "invp");
    w.send(c, "dper", fields![("PERS", "Second")]);
    assert_eq!(w.c(c).l("dper").g("RESULT"), "0");
}

// rooms and chat: the room hears it, a private message reaches one, a flood is muted
#[test]
fn rooms_and_chat() {
    let mut w = world();
    let (ga, gb) = (w.client("10.77.0.2"), w.client("10.77.0.3"));
    let a = w.login(ga, "Alpha");
    let b = w.login(gb, "Bravo");
    w.send(a, "move", fields![("NAME", "Lounge")]);
    w.send(b, "move", fields![("NAME", "Lounge")]);
    assert_eq!(w.c(b).l("move").g("COUNT"), "2");
    assert_eq!(w.c(b).l("+usr").g("N"), "Alpha");
    assert_eq!(w.c(a).l("+usr").g("A"), "10.77.0.3");
    w.cm(a).take();
    w.cm(b).take();
    w.send(a, "mesg", fields![("TEXT", "hello \u{9b} there\u{7}")]);
    assert_eq!(w.c(b).l("+msg").g("T"), "hello there");
    assert_eq!(w.c(b).l("+msg").g("N"), "Alpha");
    w.send(b, "mesg", fields![("TEXT", "psst"), ("PRIV", "alpha")]);
    assert_eq!(w.c(a).l("+msg").g("F"), "P");
    assert_eq!(w.c(a).l("+msg").g("T"), "psst");
    for i in 0..8 {
        w.send(a, "mesg", fields![("TEXT", format!("spam {i}"))]);
    }
    assert_eq!(w.c(a).l("mesg").code, "mute");
    assert!(w.logged("muted"));
    let heard = w.c(b).msgs.iter().filter(|x| x.kind == "+msg" && x.get("N") == Some("Alpha")).count();
    assert!(heard <= 6, "heard {heard}");
    w.send(a, "mesg", fields![("TEXT", "x".repeat(1000))]);
    w.send(b, "move", fields![("NAME", "")]);
    assert_eq!(w.c(a).l("+usr").g("T"), "1", "Bravo left the room");
}

// a line that reads as a slur reaches only its sender (three mute it), a blocked persona is not heard, and a
// complaint logs what the player said where the complainer could hear it
#[test]
fn slurs_blocks_and_complaints() {
    // a minute, not ten: a session says nothing for two at most
    let limits = Limits { slur_window_ms: 60_000.0, slur_mute_ms: 60_000.0, said_ms: 60_000.0, ..Limits::default() };
    let mut w = World::new(Opts { limits, ..Opts::default() });
    let (a, _) = messenger(&mut w, "10.77.0.2", "Alpha");
    let (b, bm) = messenger(&mut w, "10.77.0.3", "Bravo");
    let (c, _) = messenger(&mut w, "10.77.0.4", "Charlie");
    for x in [a, b, c] {
        w.send(x, "move", fields![("NAME", "Lounge")]);
    }
    let heard = |w: &World, x: C, t: &str| w.c(x).msgs.iter().any(|m| m.kind == "+msg" && m.get("T") == Some(t));
    // the sender sees its line as said; the room does not
    w.send(a, "mesg", fields![("TEXT", "nate higgers")]);
    assert!(heard(&w, a, "nate higgers") && !heard(&w, b, "nate higgers") && !heard(&w, c, "nate higgers"));
    assert!(w.logged("\"Alpha\": a line with \"nate higgers\" in it, shown to no one else"));
    // swearing goes through
    w.advance(2000.0);
    w.send(a, "mesg", fields![("TEXT", "merci kurwa")]);
    assert!(heard(&w, b, "merci kurwa") && heard(&w, c, "merci kurwa"));
    // private lines: one to Charlie, one to Bravo
    w.advance(2000.0);
    w.send(a, "mesg", fields![("TEXT", "psst charlie"), ("PRIV", "Charlie")]);
    w.send(a, "mesg", fields![("TEXT", "psst bravo"), ("PRIV", "bravo")]);
    assert!(heard(&w, c, "psst charlie") && heard(&w, b, "psst bravo"));
    // the third line that reads as a slur in the window mutes the sender
    w.advance(2000.0);
    w.send(a, "mesg", fields![("TEXT", "n i g g e r")]);
    assert!(!w.logged("muted for 60 s"));
    w.advance(2000.0);
    w.send(a, "mesg", fields![("TEXT", "SIEG HEIL")]);
    assert!(w.logged("\"Alpha\": a line with \"SIEG HEIL\" in it, shown to no one else; muted for 60 s"));
    w.advance(2000.0);
    w.send(a, "mesg", fields![("TEXT", "hello?")]);
    assert_eq!(w.c(a).l("mesg").code, "mute");
    assert!(!heard(&w, b, "hello?"));
    // Bravo blocks Charlie (EA Messenger): Charlie's lines reach Alpha, not Bravo
    w.send(bm, "RADD", fields![("USER", "Charlie"), ("LIST", "I"), ("ID", 9)]);
    w.send(c, "mesg", fields![("TEXT", "hi all")]);
    assert!(heard(&w, a, "hi all") && !heard(&w, b, "hi all") && heard(&w, c, "hi all"));
    w.advance(2000.0);
    w.send(c, "mesg", fields![("TEXT", "hi bravo"), ("PRIV", "Bravo")]);
    assert!(!heard(&w, b, "hi bravo"));
    // ... until Bravo takes the block back
    w.send(bm, "RDEL", fields![("USER", "Charlie"), ("LIST", "I"), ("ID", 10)]);
    w.advance(2000.0);
    w.send(c, "mesg", fields![("TEXT", "hi again")]);
    assert!(heard(&w, b, "hi again") && !w.core.buddies.blocked("charlie"));
    // Bravo's complaint about Alpha: its address, its last five lines but the private one to Charlie
    w.send(b, "rept", fields![("PERS", "Alpha"), ("LANG", "EN"), ("TYPE", "0")]);
    assert_eq!(w.c(b).l("rept").g("TEXT"), "Report complete");
    let line = w.text().into_iter().find(|l| l.contains("a complaint about")).expect("the complaint's line");
    assert_eq!(
        line,
        "[online] 10.77.0.3 \"Bravo\": a complaint about 10.77.0.2 \"Alpha\", who said: \"merci kurwa\" \"psst bravo\" \
         \"n i g g e r\" (shown to no one) \"SIEG HEIL\" (shown to no one)"
    );
    // a minute on, nothing is kept, and Alpha may speak again
    w.advance(61_000.0);
    assert!(!w.core.lobby.said.contains_key("alpha"));
    w.send(c, "rept", fields![("PERS", "Alpha")]);
    assert!(w.logged("\"Charlie\": a complaint about 10.77.0.2 \"Alpha\" (no lines of its kept)"));
    w.send(a, "mesg", fields![("TEXT", "sorry")]);
    assert!(heard(&w, b, "sorry"));
}

// a game: create, list, join, ready, start; +ses names every player by virtual address
#[test]
fn a_game() {
    let mut w = world();
    let (ga, gb) = (w.client("10.77.0.2"), w.client("10.77.0.3"));
    let a = w.login(ga, "Alpha");
    let b = w.login(gb, "Bravo");
    let p = "TRACK=4123\nDIR=0\nLAPS=2";
    w.send(
        a,
        "gcre",
        fields![("NAME", "088.Alpha"), ("MAXSIZE", 4), ("MINSIZE", 2), ("CUSTFLAGS", 134217971), ("SYSFLAGS", 0), ("PARAMS", p)],
    );
    let cre = w.c(a).l("gcre");
    assert_eq!(cre.code, "");
    assert_eq!(cre.g("HOST"), "Alpha");
    assert_eq!(cre.g("ADDR0"), "10.77.0.2");
    assert_eq!(cre.g("PARAMS"), p);
    assert!(w.c(a).last("+mgm").is_some() && w.c(a).l("+who").g("F") == "G");
    // the game list: a Filter of another race type lists it too
    w.send(
        b,
        "gsea",
        fields![("START", 0), ("COUNT", 20), ("CUSTFLAGS", 67109107), ("CUSTMASK", 67109363), ("SYSFLAGS", 0), ("SYSMASK", 786432)],
    );
    assert_eq!(w.c(b).l("gsea").g("COUNT"), "1");
    assert_eq!(w.c(b).l("+gam").g("NAME"), "088.Alpha");
    w.send(b, "gjoi", fields![("NAME", "088.Alpha")]);
    let j = w.c(b).l("gjoi");
    assert_eq!((j.g("COUNT"), j.g("OPPO1"), j.g("ADDR1")), ("2", "Bravo", "10.77.0.3"));
    assert_eq!(w.c(a).l("+mgm").g("COUNT"), "2");
    // the game room's chat
    w.send(b, "mesg", fields![("TEXT", "hi room"), ("ATTR", "G")]);
    assert_eq!(w.c(a).l("+msg").g("T"), "hi room");
    assert_eq!(w.c(a).l("+msg").g("N"), "Bravo");
    w.send(b, "gset", fields![("NAME", "088.Alpha"), ("USERFLAGS", 134217728)]);
    assert_eq!(w.c(a).l("+mgm").g("OPFLAG1"), "134217728");
    w.send(a, "gset", fields![("NAME", "088.Alpha"), ("USERFLAGS", 134217728)]);
    w.send(b, "gsta", fields![("NAME", "088.Alpha")]);
    assert_eq!(w.c(b).l("gsta").code, "host", "only the creator starts");
    w.send(a, "gsta", fields![("NAME", "088.Alpha")]);
    let (sa, sb) = (w.c(a).l("+ses").clone(), w.c(b).l("+ses").clone());
    assert_eq!(sa.g("SELF"), "Alpha");
    assert_eq!(sb.g("SELF"), "Bravo");
    assert_eq!(sa.g("SEED"), sb.g("SEED"));
    assert_eq!(sa.g("AUTH"), sb.g("AUTH"));
    assert!(!sa.g("AUTH").is_empty());
    assert_eq!([sb.g("ADDR0"), sb.g("LADDR0"), sb.g("ADDR1")], ["10.77.0.2", "10.77.0.2", "10.77.0.3"]);
    assert_eq!(sb.g("SYSFLAGS").parse::<u32>().unwrap() & (1 << 19), 1 << 19, "started");
    // the game closes its lobby links at the start: the race (the game record) goes on
    w.close(a);
    w.close(b);
    assert_eq!(w.core.lobby.games.len(), 1);
    assert_eq!(w.core.lobby.by_persona.len(), 0);
    // after the race the players log in again, with the same names
    let ga2 = w.client("10.77.0.2");
    let a2 = w.login(ga2, "Alpha");
    assert_eq!(w.c(a2).l("pers").code, "");
    w.send(a2, "gsea", fields![("START", 0), ("COUNT", 20), ("CUSTFLAGS", 0), ("CUSTMASK", 0), ("SYSFLAGS", 0), ("SYSMASK", 0)]);
    assert_eq!(w.c(a2).l("gsea").g("COUNT"), "0", "a started race is not listed");
    w.advance(31.0 * 60_000.0);
    assert_eq!(w.core.lobby.games.len(), 0, "forgotten after raceMs");
}

// game passwords: listed with the password bit, asked for on every join (by name or id), in no reply, log or record
#[test]
fn game_passwords() {
    const PASSWORD: u32 = 1 << 16;
    let mut w = world();
    let cs: Vec<C> = ["Alpha", "Bravo", "Charlie"]
        .iter()
        .enumerate()
        .map(|(i, n)| {
            let g = w.client(&format!("10.77.0.{}", i + 2));
            w.login(g, n)
        })
        .collect();
    let (a, b, c) = (cs[0], cs[1], cs[2]);
    let sys = |m: &super::world::M| m.g("SYSFLAGS").parse::<u32>().unwrap();
    let pw = "Room Secret:%=42";
    // the game's gcre: the password, SYSFLAGS without the bit (SPEED2.EXE 0x58d580)
    w.send(a, "gcre", fields![("NAME", "Locked"), ("SYSFLAGS", 0), ("PASS", pw), ("PARAMS", "TRACK=4011")]);
    assert_eq!(sys(w.c(a).l("gcre")) & PASSWORD, PASSWORD);
    let id: u64 = w.c(a).l("gcre").g("IDENT").parse().unwrap();
    w.send(b, "gsea", fields![("START", 0), ("COUNT", 20)]);
    assert_eq!(sys(w.c(b).l("+gam")) & PASSWORD, PASSWORD, "listed as a game with a password");
    // a join refused leaves the player in its own game
    w.send(b, "gcre", fields![("NAME", "Other"), ("PARAMS", "TRACK=4011")]);
    let other: u64 = w.c(b).l("gcre").g("IDENT").parse().unwrap();
    let in_game = |w: &World, name: &str| {
        let s = w.core.lobby.sessions.iter().find(|s| w.core.lobby.s(**s).persona.as_deref() == Some(name)).unwrap();
        w.core.lobby.s(*s).game
    };
    for f in [
        fields![("NAME", "Locked")],
        fields![("IDENT", id), ("PASS", "")],
        fields![("NAME", "Locked"), ("PASS", "wrong")],
        fields![("IDENT", id), ("PASS", pw.to_lowercase())],
    ] {
        w.advance(1000.0);
        w.send(b, "gjoi", f);
        assert_eq!(w.c(b).l("gjoi").code, "pass");
        assert_eq!(w.core.lobby.games[&id].players.len(), 1);
        assert_eq!(in_game(&w, "Bravo"), Some(other));
    }
    // a gset clears no password, the creator's or a player's
    w.send(a, "gset", fields![("SYSFLAGS", 0), ("PASS", "")]);
    w.advance(1000.0);
    w.send(b, "gjoi", fields![("IDENT", id), ("PASS", pw)]);
    assert_eq!(w.c(b).l("gjoi").code, "");
    assert_eq!(w.c(b).l("gjoi").g("COUNT"), "2");
    assert!(!w.core.lobby.games.contains_key(&other), "Bravo's own game ended as Bravo left it");
    w.send(b, "gset", fields![("SYSFLAGS", 0), ("PASS", "")]);
    w.send(c, "gjoi", fields![("NAME", "Locked")]);
    assert_eq!(w.c(c).l("gjoi").code, "pass");
    w.send(c, "gjoi", fields![("NAME", "Locked"), ("PASS", pw)]);
    assert_eq!(w.c(c).l("gjoi").code, "");
    // a join into the game one is in changes nothing (the creator's does not end it)
    w.send(a, "gjoi", fields![("NAME", "Locked"), ("PASS", pw)]);
    assert_eq!(w.c(a).l("gjoi").g("COUNT"), "3");
    w.advance(1000.0);
    w.send(b, "gjoi", fields![("IDENT", id), ("PASS", pw)]);
    let g = &w.core.lobby.games[&id];
    assert_eq!(g.players.iter().map(|p| p.name.as_str()).collect::<Vec<_>>(), ["Alpha", "Bravo", "Charlie"]);
    assert_eq!(g.sys_flags & PASSWORD, PASSWORD);
    // the password: in no record, log line or message
    assert!(!format!("{g:?}").contains(pw));
    assert!(!w.text().join("\n").contains(pw));
    for x in [a, b, c] {
        for m in &w.c(x).msgs {
            assert!(!m.f.has("PASS") && !m.f.has("PASSWORD"), "{} has a password field", m.kind);
        }
        // (as sent or escaped: "Room Secret%3a%25%3d42")
        assert!(!w.c(x).raw.windows(11).any(|r| r == b"Room Secret"));
    }
}

// game passwords: an empty one is none, and the bit alone (from a creator, not the game) makes no password game
#[test]
fn game_passwords_open() {
    const PASSWORD: u32 = 1 << 16;
    let mut w = world();
    let (ga, gb) = (w.client("10.77.0.2"), w.client("10.77.0.3"));
    let a = w.login(ga, "Alpha");
    let b = w.login(gb, "Bravo");
    for (name, f) in [("Empty", fields![("PASS", "")]), ("Bit", fields![("SYSFLAGS", PASSWORD)])] {
        let mut gf = fields![("NAME", name), ("PARAMS", "TRACK=4011")];
        gf.extend(f);
        w.send(a, "gcre", gf);
        assert_eq!(w.c(a).l("gcre").g("SYSFLAGS").parse::<u32>().unwrap() & PASSWORD, 0, "{name}");
        w.advance(1000.0);
        w.send(b, "gjoi", fields![("NAME", name)]);
        assert_eq!(w.c(b).l("gjoi").code, "", "{name}");
        w.send(b, "glea", vec![]);
        w.send(a, "gdel", vec![]);
        w.advance(1000.0);
    }
}

// the game list: every open game but a private one, the Filter's games first; once the player set the Filter
// (code "filt"), only those, unless it asks for its defaults
#[test]
fn the_game_list() {
    let mut w = world();
    let cs: Vec<C> = ["Alpha", "Bravo", "Charlie", "Delta", "Echo"]
        .iter()
        .enumerate()
        .map(|(i, n)| {
            let g = w.client(&format!("10.77.0.{}", i + 2));
            w.login(g, n)
        })
        .collect();
    let (a, b, c, d, e) = (cs[0], cs[1], cs[2], cs[3], cs[4]);
    // the game's default search: race type 0, Collision Detection and Race Breaker on, no car restriction, unranked,
    // not private (CUSTMASK bit 8)
    let search =
        || fields![("START", 0), ("COUNT", 20), ("CUSTFLAGS", 0x40000f3), ("CUSTMASK", 0x40001f3), ("SYSFLAGS", 0), ("SYSMASK", 0xc0000)];
    w.send(e, "gsea", search());
    w.send(a, "gcre", fields![("NAME", "no collision"), ("CUSTFLAGS", 0x40000f2), ("PARAMS", "TRACK=4011")]);
    w.send(b, "gcre", fields![("NAME", "private"), ("CUSTFLAGS", 0x40001f3), ("PARAMS", "TRACK=4011")]);
    w.send(c, "gcre", fields![("NAME", "as asked"), ("CUSTFLAGS", 0x40000f3), ("PARAMS", "TRACK=4011")]);
    w.send(d, "gcre", fields![("NAME", "ranked sprint"), ("CUSTFLAGS", 0x80000f3), ("SYSFLAGS", 1 << 18), ("PARAMS", "TRACK=4011")]);
    w.advance(1000.0);
    let names = |w: &mut World, kind: &str| -> Vec<String> {
        w.cm(e).take().iter().filter(|m| m.kind == kind).map(|m| m.g("NAME").to_string()).collect()
    };
    let mut agm = names(&mut w, "+agm");
    agm.sort();
    assert_eq!(agm, ["as asked", "no collision", "ranked sprint"], "a private game is not announced");
    w.send(e, "gsea", search());
    assert_eq!(w.c(e).l("gsea").g("COUNT"), "3");
    assert_eq!(names(&mut w, "+gam"), ["as asked", "no collision", "ranked sprint"]);
    let mut s = fields![("START", 1), ("COUNT", 1)];
    s.extend(search().into_iter().skip(2));
    w.send(e, "gsea", s);
    assert_eq!(names(&mut w, "+gam"), ["no collision"]);
    // ranked sprints: unset (the settings of a game made or joined) the others come after; set, only those; set to
    // the defaults, all of them again
    let sprint = || {
        fields![("START", 0), ("COUNT", 20), ("CUSTFLAGS", 0x80000f3), ("CUSTMASK", 0x80001f3), ("SYSFLAGS", 1 << 18), ("SYSMASK", 0xc0000)]
    };
    w.send(e, "gsea", sprint());
    assert_eq!(names(&mut w, "+gam"), ["ranked sprint", "no collision", "as asked"]);
    w.send_raw(e, &encode_frame("gsea", "filt", &sprint()).unwrap());
    assert_eq!(w.c(e).l("gsea").g("COUNT"), "1");
    assert_eq!(names(&mut w, "+gam"), ["ranked sprint"]);
    w.send_raw(e, &encode_frame("gsea", "filt", &search()).unwrap());
    assert_eq!(names(&mut w, "+gam"), ["as asked", "no collision", "ranked sprint"]);
    // a private game's players come by invitation: by its name
    w.send(e, "gjoi", fields![("NAME", "private")]);
    assert_eq!(w.c(e).l("gjoi").code, "");
}

// the game list in one search: the config sets the game's page (GAMES_PER_UPDATE), its search asks for that many, and
// gets every open game, oldest first; past the page, the next one
#[test]
fn the_game_list_in_one_search() {
    let mut w = world();
    let hosts: Vec<C> = (0..210)
        .map(|i| {
            let g = w.client(&format!("10.77.{}.{}", 1 + i / 200, 2 + i % 200));
            w.login(g, &format!("Host{i}"))
        })
        .collect();
    let g = w.client("10.77.0.2");
    let e = w.login(g, "Looker");
    assert_eq!(w.c(e).l("news").g("GAMES_PER_UPDATE"), "200");
    for (i, &h) in hosts.iter().enumerate() {
        w.send(h, "gcre", fields![("NAME", format!("{i}00.Host{i}")), ("CUSTFLAGS", 0x40000f3), ("PARAMS", "TRACK=4011")]);
    }
    let names = |w: &mut World| -> Vec<String> {
        w.cm(e).take().iter().filter(|m| m.kind == "+gam").map(|m| m.g("NAME").to_string()).collect()
    };
    let search = |s: i32, n: i32| {
        fields![("START", s), ("COUNT", n), ("CUSTFLAGS", 0x40000f3), ("CUSTMASK", 0x40001f3), ("SYSFLAGS", 0), ("SYSMASK", 0xc0000)]
    };
    let made = |r: std::ops::Range<usize>| -> Vec<String> { r.map(|i| format!("{i}00.Host{i}")).collect() };
    w.cm(e).take();
    w.send(e, "gsea", search(0, 200));
    assert_eq!(w.c(e).l("gsea").g("COUNT"), "200");
    assert_eq!(names(&mut w), made(0..200), "a page of 200, oldest first");
    w.send(e, "gsea", search(200, 200));
    assert_eq!(names(&mut w), made(200..210));
    w.send(e, "gsea", search(0, 1000));
    assert_eq!(w.c(e).l("gsea").g("COUNT"), "200", "never more than a page");
}

// six players: a game asked for 8 takes 6, the seventh finds it full; +ses names all six
#[test]
fn six_players() {
    let mut w = world();
    let names = ["Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf"];
    let cs: Vec<C> = names
        .iter()
        .enumerate()
        .map(|(i, n)| {
            let g = w.client(&format!("10.77.0.{}", i + 2));
            w.login(g, n)
        })
        .collect();
    w.send(cs[0], "gcre", fields![("NAME", "six"), ("MAXSIZE", 8), ("PARAMS", "TRACK=4123")]);
    assert_eq!(w.c(cs[0]).l("gcre").g("MAXSIZE"), "6");
    for c in &cs[1..6] {
        w.send(*c, "gjoi", fields![("NAME", "six")]);
        assert_eq!(w.c(*c).l("gjoi").code, "");
    }
    w.send(cs[6], "gjoi", fields![("NAME", "six")]);
    assert_eq!(w.c(cs[6]).l("gjoi").code, "full");
    w.send(cs[0], "gsta", fields![("NAME", "six")]);
    let s = w.c(cs[5]).l("+ses");
    assert_eq!((s.g("COUNT"), s.g("OPPO5"), s.g("ADDR5")), ("6", "Foxtrot", "10.77.0.7"));
}

// Start Race before every player is ready: the room counts down, the players still not ready are kicked, the race
// starts
#[test]
fn start_race_countdown_kick() {
    const READY: u32 = 1 << 27;
    const COUNTDOWN: u32 = 1 << 12;
    const RANKED: u32 = 1 << 18;
    const ACTIVE: u32 = 1 << 19;
    let mut w = world();
    let cs: Vec<C> = ["Alpha", "Bravo", "Charlie", "Delta"]
        .iter()
        .enumerate()
        .map(|(i, n)| {
            let g = w.client(&format!("10.77.0.{}", i + 2));
            w.login(g, n)
        })
        .collect();
    let (a, b, c, d) = (cs[0], cs[1], cs[2], cs[3]);
    w.send(a, "gcre", fields![("NAME", "g"), ("MAXSIZE", 4), ("MINSIZE", 2), ("PARAMS", "TRACK=4011")]);
    for x in [b, c, d] {
        w.send(x, "gjoi", fields![("NAME", "g")]);
    }
    w.send(b, "gset", fields![("NAME", "g"), ("USERFLAGS", READY)]);
    let sys = |m: &super::world::M| m.g("SYSFLAGS").parse::<u32>().unwrap();
    // the creator's Start Race (SPEED2.EXE 0x58aa20): the game's SYSFLAGS with the countdown, the creator ready; only
    // the countdown bit is the creator's to set
    let cur = sys(w.c(a).l("+mgm"));
    w.send(a, "gset", fields![("NAME", "g"), ("SYSFLAGS", cur | COUNTDOWN | RANKED | ACTIVE), ("USERFLAGS", READY)]);
    assert_eq!(sys(w.c(a).l("gset")), COUNTDOWN);
    for x in [b, c, d] {
        assert_eq!(sys(w.c(x).l("+mgm")), COUNTDOWN, "every game in the room counts down");
    }
    assert_eq!(w.c(a).l("+mgm").g("OPFLAG0"), READY.to_string());
    w.send(c, "gset", fields![("NAME", "g"), ("SYSFLAGS", 0)]);
    assert_eq!(sys(w.c(c).l("gset")), COUNTDOWN, "a joiner does not stop it");
    // a start that fails clears it (0x58ab40), and Start Race sets it again
    w.send(a, "gset", fields![("NAME", "g"), ("SYSFLAGS", 0), ("USERFLAGS", 0)]);
    assert_eq!(sys(w.c(b).l("+mgm")), 0);
    w.send(a, "gset", fields![("NAME", "g"), ("SYSFLAGS", COUNTDOWN), ("USERFLAGS", READY)]);
    // the countdown's end (0x58a7e0): the players not ready in one KICK list, then the start
    w.send(a, "gset", fields![("NAME", "g"), ("KICK", "Charlie,Delta")]);
    for (x, n) in [(c, "Charlie"), (d, "Delta")] {
        // the game leaves the room on the game's record after its own without the game (SPEED2.EXE 0x58dab0)
        let msgs = &w.c(x).msgs;
        let (who, mgm) = (&msgs[msgs.len() - 2], &msgs[msgs.len() - 1]);
        assert_eq!((who.kind.as_str(), who.g("G")), ("+who", "0"), "{n} is out of the game");
        assert_eq!(mgm.kind, "+mgm");
        let named: Vec<&str> = mgm.f.0.iter().filter(|(k, _)| k.starts_with("OPPO")).filter_map(|(_, v)| v.as_deref()).collect();
        assert!(named.contains(&"Bravo") && !named.contains(&n), "{n}'s game record without {n}: {named:?}");
    }
    assert_eq!(w.c(b).l("+mgm").g("COUNT"), "2");
    w.send(a, "gsta", fields![("NAME", "g")]);
    assert_eq!(w.c(a).l("gsta").code, "");
    let s = w.c(b).l("+ses");
    assert_eq!(s.g("COUNT"), "2");
    assert_eq!([s.g("OPPO0"), s.g("OPPO1")], ["Alpha", "Bravo"]);
    assert!(w.c(c).last("+ses").is_none() && w.c(d).last("+ses").is_none());
}

// leaving: a joiner leaves, the creator leaves, the creator deletes; a browser that goes
#[test]
fn leaving() {
    let mut w = world();
    let (ga, gb) = (w.client("10.77.0.2"), w.client("10.77.0.3"));
    let a = w.login(ga, "Alpha");
    let b = w.login(gb, "Bravo");
    w.send(a, "gcre", fields![("NAME", "g1"), ("MAXSIZE", 2), ("PARAMS", "TRACK=1")]);
    w.send(b, "gjoi", fields![("NAME", "g1")]);
    let gc = w.client("10.77.0.4");
    let c = w.login(gc, "Charlie");
    w.send(c, "gjoi", fields![("NAME", "g1")]);
    assert_eq!(w.c(c).l("gjoi").code, "full");
    w.send(b, "glea", vec![]);
    assert_eq!(w.c(a).l("+mgm").g("COUNT"), "1");
    w.send(b, "gjoi", fields![("NAME", "g1")]);
    w.send(a, "gdel", vec![]);
    assert_eq!(w.c(b).l("+mgm").g("IDENT"), "1");
    assert_eq!(w.c(b).l("+mgm").get("COUNT"), None, "IDENT only: deleted");
    assert_eq!(w.core.lobby.games.len(), 0);
    w.send(a, "gcre", fields![("NAME", "g2"), ("PARAMS", "TRACK=1")]);
    w.send(b, "gjoi", fields![("NAME", "g2")]);
    // the creator's browser leaves the switch
    w.peer_gone(ip("10.77.0.2"));
    assert_eq!(w.core.lobby.games.len(), 0);
    assert_eq!(w.c(b).l("+mgm").get("COUNT"), None);
    assert_eq!(w.core.lobby.sessions.len(), 2);
}

// limits: message flood, sessions per browser, frame size, silence
#[test]
fn limits() {
    let mut w = World::new(Opts { limits: Limits { per_vip: 2.0, ..Default::default() }, ..Default::default() });
    let g = w.client("10.77.0.2");
    let a = w.login(g, "Alpha");
    for _ in 0..60 {
        w.send(a, "~png", fields![("TIME", 1)]);
    }
    assert!(w.c(a).reset, "more than 40 messages at once: dropped");
    w.login(g, "Beta");
    w.login(g, "Gamma");
    let d = w.login(g, "Delta");
    assert!(w.c(d).reset, "a third session from one browser: refused");
    let mut w2 = world();
    let h = w2.client("10.77.0.5");
    let big = w2.connect(h, PORT_LOBBY);
    w2.send(big, "mesg", fields![("TEXT", "x".repeat(9000))]);
    assert!(w2.c(big).reset, "a message over 8 KB: dropped");
    let odd = w2.login(h, "Oddball");
    let cid = w2.c(odd).cid;
    let mut data = b"a\x01bc".to_vec();
    data.extend_from_slice(&[0, 0, 0, 0, 0, 0, 0, 13, 0]);
    w2.raw(h, &lan_frame(4, h, VHOST_VIP, odd.1, PORT_LOBBY, DATA, cid, &data));
    assert!(w2.c(odd).reset, "a kind that is not four letters: dropped");
    let quiet = w2.login(h, "Quiet");
    w2.advance(60_000.0);
    w2.advance(61_000.0);
    assert!(w2.c(quiet).reset, "silent for 120 s: dropped");
    assert!(w2.c(quiet).msgs.iter().any(|m| m.kind == "~png"), "pinged first");
}

/// a real race report (SPEED2.EXE, a 2-lap circuit here: index 1 first in 186.82 s, index 0 second in 187.67 s)
pub const RESU: &str = "AwIAAQ+uAAIBAQEVQAADJ0M60exCuGZnQflKqkIogLdAsZmaQU2tD0I5HwoAAAAAAAAAAAAAAAAAAAAAAAAAAAIBFUAAAOhDO6uGQreo9kH44idCKNlYQL3CkEFciThCPgV4AAAAAAAAAAAAAAAAAAAAAAAAAA==";

fn report(w: &mut World, c: C, auth: &str) {
    w.send(
        c,
        "rank",
        fields![
            ("AUTH", auth),
            ("REPT", "x"),
            ("RESU", RESU),
            ("VENUE", "NFSUG2"),
            ("NAME0", "Alpha"),
            ("TEAM0", "NFSUG2"),
            ("NAME1", "Bravo"),
            ("TEAM1", "NFSUG2")
        ],
    );
}

fn stat_of(m: &super::world::M) -> Vec<u32> {
    m.g("STAT").split(',').take(38).map(|x| u32::from_str_radix(x, 16).unwrap()).collect()
}

// results: the race report counts once, is checked, and shows in the career and the rankings
#[test]
fn results() {
    let mut w = world();
    let (ga, gb) = (w.client("10.77.0.2"), w.client("10.77.0.3"));
    let a = w.login(ga, "Alpha");
    let b = w.login(gb, "Bravo");
    w.send(a, "gcre", fields![("NAME", "r1"), ("PARAMS", "TRACK=4014")]);
    w.send(b, "gjoi", fields![("NAME", "r1")]);
    w.send(a, "gsta", fields![("NAME", "r1")]);
    let auth = w.c(a).l("+ses").g("AUTH").to_string();
    w.close(a);
    w.close(b);
    // too early: 186 s of racing cannot have happened 10 s after the start
    w.advance(10_000.0);
    let gb2 = w.client("10.77.0.3");
    let b2 = w.login(gb2, "Bravo");
    report(&mut w, b2, &auth);
    assert_eq!(w.c(b2).l("rank").g("RANK"), "Unranked");
    assert!(w.logged("not taken: a race of 10.0 s (a circuit race takes 20 s at least)"), "{}", w.text().join("\n"));
    // 30 s after the start: a race that long, but not 186 s of it
    w.advance(20_000.0);
    report(&mut w, b2, &auth);
    assert!(w.logged("not taken: a time the race has not had"));
    assert_eq!(w.core.stats.board(0, false).len(), 0);
    assert!(w.core.stats.rec("Bravo", false).is_none(), "no records from a report not taken");
    // after the race: Bravo's report alone decides nothing (a report not taken was not his one report); Alpha's,
    // from another browser with the same places, makes the result
    w.advance(200_000.0);
    let gb3 = w.client("10.77.0.3");
    let b3 = w.login(gb3, "Bravo");
    report(&mut w, b3, &auth);
    assert!(!w.logged("\"r1\" (circuit"), "one report alone");
    assert_eq!(w.core.stats.board(0, false).len(), 0);
    let ga2 = w.client("10.77.0.2");
    let a2 = w.login(ga2, "Alpha");
    report(&mut w, a2, &auth);
    assert!(w.logged("\"r1\" (circuit, unranked): 1 Bravo 3:06.82, 2 Alpha 3:07.67"), "{}", w.text().join("\n"));
    report(&mut w, a2, &auth);
    assert_eq!(w.core.stats.board(0, false).iter().map(|r| r.races).sum::<f64>(), 2.0, "counted once");
    assert_eq!(w.core.lobby.games.len(), 0, "both players reported: the race is over");
    // a report for no race of this server's
    w.send(a2, "rank", fields![("AUTH", "f00"), ("RESU", RESU), ("NAME0", "Alpha"), ("NAME1", "Bravo")]);
    assert!(w.text().last().unwrap().contains("no race of this server"));
    // the career: unranked win for Bravo (circuit block: values 9..13 = rating, ranked W/L, unranked W/L)
    w.send(a2, "user", fields![("PERS", "Bravo")]);
    let stat = stat_of(w.c(a2).l("user"));
    assert_eq!(stat.len(), 38);
    assert_eq!(stat[8..13], [0, 0, 0, 1, 0]);
    // the reporter got its career again right after its report (the game asked at the login, before it)
    assert!(w.c(a2).msgs.iter().any(|m| m.kind == "user" && m.get("PERS") == Some("Alpha")));
    w.send(a2, "user", fields![("PERS", "Alpha")]);
    assert_eq!(stat_of(w.c(a2).l("user"))[8..13], [0, 0, 0, 0, 1]);
    // the records (Top Speed Achieved in mm/s): each player's from its own entry in its own report (both sent the
    // same one: Bravo is index 1, 42.126 m/s; Alpha index 0, 42.212 m/s)
    assert_eq!(stat_of(w.c(a2).l("user"))[3..8], [42212, 0, 0, 0, 0]);
    w.send(a2, "user", fields![("PERS", "Bravo")]);
    assert_eq!(stat_of(w.c(a2).l("user"))[3..8], [42126, 0, 0, 0, 0]);
    // the rankings: the circuit board, and one's own row
    w.send(a2, "snap", fields![("INDEX", 4), ("CHAN", 6), ("START", 0), ("RANGE", 100)]);
    assert_eq!(w.c(a2).l("snap").g("COUNT"), "2");
    let rows: Vec<[String; 3]> =
        w.c(a2).msgs.iter().filter(|m| m.kind == "+snp").map(|m| [m.g("N").into(), m.g("R").into(), m.g("S").into()]).collect();
    assert_eq!(rows, [["Bravo", "1", "0,1,0"], ["Alpha", "2", "0,0,1"]]);
    w.send(a2, "snap", fields![("INDEX", 4), ("CHAN", 12), ("RANGE", 1), ("FIND", "$")]);
    assert_eq!(w.c(a2).l("snap").g("COUNT"), "1");
    assert_eq!(w.c(a2).l("+snp").g("N"), "Alpha");
    w.send(a2, "snap", fields![("INDEX", 4), ("CHAN", 7), ("START", 0), ("RANGE", 100)]);
    assert_eq!(w.c(a2).l("snap").g("COUNT"), "0", "no sprint races yet");
    // points in P are decimal, in S hex: 28 ranked wins, 2800 points (hex af0: the game read P "af0" as 0)
    for _ in 0..28 {
        let players = ["Alpha", "Bravo"].iter().enumerate().map(|(i, n)| RacePlayer { name: n.to_string(), place: i as u32 + 1, time: 60.0 });
        w.core.stats.add_race(&Race { kind: 4, ranked: true, track: 4011, players: players.collect() });
    }
    w.send(a2, "snap", fields![("INDEX", 4), ("CHAN", 16), ("RANGE", 1), ("FIND", "$")]);
    let row = w.c(a2).l("+snp");
    assert_eq!([row.g("N"), row.g("R"), row.g("P"), row.g("S")], ["Alpha", "1", "2800", "af0,1c,0"]);
}

// the rankings: the screens' channels go in their own order, Drift before Drag (SPEED2.EXE 0x58ce30, 0x5826e0): the
// overall boards 6..11, one's own row 12..17, the month's leaders 18..23; last month's (24..29) are empty
#[test]
fn rankings_channels() {
    let mut w = world();
    let g = w.client("10.77.0.2");
    let a = w.login(g, "Alpha");
    let add = |w: &mut World, kind: u32, names: [&str; 2]| {
        let players = names.iter().enumerate().map(|(i, n)| RacePlayer { name: n.to_string(), place: i as u32 + 1, time: 60.0 });
        w.core.stats.add_race(&Race { kind, ranked: true, track: 4011, players: players.collect() });
    };
    add(&mut w, 3, ["Alpha", "Drifter"]);
    add(&mut w, 2, ["Dragger", "Other"]);
    let rows = |w: &mut World, chan: i32, own: bool| -> Vec<String> {
        let from = w.c(a).msgs.len();
        let mut f = fields![("INDEX", 21), ("CHAN", chan), ("START", 0), ("RANGE", if own { 1 } else { 100 })];
        if own {
            f.extend(fields![("FIND", "$")]);
        }
        w.send(a, "snap", f);
        w.c(a).msgs[from..].iter().filter(|m| m.kind == "+snp").map(|m| format!("{} {}", m.g("N"), m.g("P"))).collect()
    };
    assert_eq!(rows(&mut w, 8, false), ["Alpha 100", "Drifter 0"], "Drift: the drift board");
    assert_eq!(rows(&mut w, 9, false), ["Dragger 100", "Other 0"], "Drag: the drag board");
    assert_eq!(rows(&mut w, 14, true), ["Alpha 100"], "one's own Drift row");
    assert!(rows(&mut w, 15, true).is_empty(), "no drag race of one's own");
    assert_eq!(rows(&mut w, 20, false), ["Alpha 100", "Drifter 0"], "the month's drift leaders");
    assert!(rows(&mut w, 26, false).is_empty(), "last month's leaders: none kept");
    assert_eq!(w.c(a).l("snap").g("TOTAL"), "0");
}

// names: a name another browser holds comes with four digits (the same for one client); its own browser takes it
// back
#[test]
fn names_digits() {
    let mut w = world();
    let g = w.client("10.77.0.2");
    let a = w.login(g, "NAME");
    assert_eq!(w.c(a).l("pers").g("PERS"), "NAME");
    let g = w.client("10.77.0.3");
    let b = w.login(g, "NAME");
    let bn = w.c(b).l("pers").g("PERS").to_string();
    assert_eq!(w.c(b).l("pers").code, "");
    assert!(digits4(&bn, "NAME"));
    assert_eq!(w.c(b).l("+who").g("N"), bn, "the lobby knows the player by it");
    assert!(w.logged(&format!("plays as \"{bn}\" (\"NAME\" is taken)")), "{}", w.text().join("\n"));
    let g = w.client("10.77.0.4");
    let c = w.login(g, "NAME");
    let cn = w.c(c).l("pers").g("PERS").to_string();
    assert!(digits4(&cn, "NAME"));
    assert_ne!(cn, bn);
    let mut keys: Vec<String> = w.core.lobby.by_persona.keys().cloned().collect();
    keys.sort();
    let mut want = vec!["name".to_string(), bn.to_lowercase(), cn.to_lowercase()];
    want.sort();
    assert_eq!(keys, want);
    // the digits take the end of a long name: still a persona name (15 characters at most)
    let g = w.client("10.77.0.5");
    w.login(g, "ABCDEFGHIJKLMNO");
    let g = w.client("10.77.0.6");
    let l = w.login(g, "ABCDEFGHIJKLMNO");
    assert!(digits4(w.c(l).l("pers").g("PERS"), "ABCDEFGHIJK"));
    // the same client logging in again: the same digits (the game asks for them now: the runtime took them)
    w.close(b);
    let g = w.client("10.77.0.3");
    let x = w.login(g, "NAME");
    assert_eq!(w.c(x).l("pers").g("PERS"), bn);
    // (the old `b` again: a FIN for a stream the host no longer has does nothing)
    w.close(b);
    let g = w.client("10.77.0.3");
    let x = w.login(g, &bn);
    assert_eq!(w.c(x).l("pers").g("PERS"), bn);
    // ... and asking for the name while its older session still plays under the digits (its game's new lobby link):
    // the same digits, from that session
    let g = w.client("10.77.0.3");
    let x = w.login(g, "NAME");
    assert_eq!(w.c(x).l("pers").g("PERS"), bn);
    assert_eq!(w.personas().iter().filter(|p| p.as_deref() == Some(bn.as_str())).count(), 1, "the old session went");
    assert_eq!(w.core.lobby.by_persona.len(), 5, "NAME and two with digits, the long name and one with digits");
    // the holder's own browser logging in again (its game's new lobby link): the name, from its old session
    let g = w.client("10.77.0.2");
    let a2 = w.login(g, "NAME");
    assert_eq!(w.c(a2).l("pers").g("PERS"), "NAME");
    assert_eq!(w.personas().iter().filter(|p| p.as_deref() == Some("NAME")).count(), 1, "the old session went");
    // a shortened name that reads as a slur is "Player" with the digits
    let g = w.client("10.77.0.7");
    w.login(g, "AbcdefghFagxyz");
    let g = w.client("10.77.0.8");
    let p = w.login(g, "AbcdefghFagxyz");
    assert!(digits4(w.c(p).l("pers").g("PERS"), "Player"), "{}", w.c(p).l("pers").g("PERS"));
}

// names: a racer keeps the name until back; another player under it gets digits and ends nothing
#[test]
fn names_racer() {
    let mut w = world();
    let (ga, gb) = (w.client("10.77.0.2"), w.client("10.77.0.3"));
    let a = w.login(ga, "Alpha");
    let b = w.login(gb, "Bravo");
    w.send(a, "gcre", fields![("NAME", "r2"), ("PARAMS", "TRACK=4014")]);
    w.send(b, "gjoi", fields![("NAME", "r2")]);
    w.send(a, "gsta", fields![("NAME", "r2")]);
    let auth = w.c(a).l("+ses").g("AUTH").to_string();
    // the game closes its lobby link for the race
    w.close(a);
    w.close(b);
    // another browser under the racer's name: digits, and its game list does not take the racer back
    let go = w.client("10.77.0.4");
    let other = w.login(go, "Alpha");
    assert!(digits4(w.c(other).l("pers").g("PERS"), "Alpha"));
    w.send(other, "gsea", fields![("START", 0), ("COUNT", 20)]);
    w.advance(200_000.0);
    let rep = |w: &mut World, c: C| {
        w.send(
            c,
            "rank",
            fields![("AUTH", auth.as_str()), ("REPT", "x"), ("RESU", RESU), ("VENUE", "NFSUG2"), ("NAME0", "Alpha"), ("NAME1", "Bravo")],
        )
    };
    let gb2 = w.client("10.77.0.3");
    let b2 = w.login(gb2, "Bravo");
    w.send(b2, "gsea", fields![("START", 0), ("COUNT", 20)]);
    rep(&mut w, b2);
    assert_eq!(w.core.lobby.games.len(), 1, "Alpha is still racing");
    // the racer, back from its own browser: its name, and its report makes the result
    let ga2 = w.client("10.77.0.2");
    let a2 = w.login(ga2, "Alpha");
    assert_eq!(w.c(a2).l("pers").g("PERS"), "Alpha");
    rep(&mut w, a2);
    assert!(w.logged("\"r2\" (circuit, unranked): 1 Bravo 3:06.82, 2 Alpha 3:07.67"), "{}", w.text().join("\n"));
    assert_eq!(w.core.lobby.games.len(), 0);
}

// web pages: the news and terms pages as web-offer scripts, nothing else
#[test]
fn web_pages() {
    let mut w = world();
    let g = w.client("10.77.0.2");
    let get = |w: &mut World, req: &str| -> (String, bool) {
        let c = w.connect(g, PORT_HTTP);
        w.send_raw(c, req.as_bytes());
        (crate::jsx::latin1_decode(&w.c(c).raw), w.c(c).fin)
    };
    let (n, fin) = get(&mut w, "GET /news HTTP/1.0\r\nUser-Agent: Custom/1.0\r\n\r\n");
    assert!(n.starts_with("HTTP/1.0 200 OK\r\n"));
    assert!(n.contains("\r\n\r\n%{ CMD=news TITLE=\"NFSU2 Online\" BTN1=\"OK\" BTN1-GOTO=\"$quit\" %}\r\n"));
    assert!(fin);
    assert!(get(&mut w, "GET /tos HTTP/1.0\r\n\r\n").0.contains("Terms of Service"));
    assert!(get(&mut w, "GET /card HTTP/1.0\r\n\r\n").0.starts_with("HTTP/1.0 404"));
    assert!(get(&mut w, "POST /news HTTP/1.0\r\n\r\n").0.starts_with("HTTP/1.0 400"));
    for p in ["/news", "/tos"] {
        let t = get(&mut w, &format!("GET {p} HTTP/1.0\r\n\r\n")).0;
        assert!(!t.contains("CMD=card") && !t.contains("CMD=prom"));
    }
}

// EA Messenger: the login by LKEY, friends, presence, messages, blocking, leaving
#[test]
fn ea_messenger() {
    let mut w = world();
    let (ga, gb) = (w.client("10.77.0.2"), w.client("10.77.0.3"));
    let la = w.login(ga, "Alpha");
    let lb = w.login(gb, "Bravo");
    let (ka, kb) = (w.lkey(la), w.lkey(lb));
    let a = w.buddy(ga, &ka);
    let b = w.buddy(gb, &kb);
    assert_eq!(w.c(a).l("AUTH").code, "");
    assert_eq!(w.c(a).l("AUTH").g("TITL"), "EA MESSENGER");
    w.send(a, "EPGT", fields![("LRSC", "PC"), ("ID", 4)]);
    assert_eq!(w.c(a).l("EPGT").g("ENAB"), "N");
    w.send(a, "RGET", fields![("LRSC", "PC"), ("LIST", "B"), ("PRES", "Y"), ("PEND", "Y"), ("ID", 1)]);
    assert_eq!([w.c(a).l("RGET").g("ID"), w.c(a).l("RGET").g("SIZE")], ["1", "0"]);
    // the Search finds Bravo, not Alpha itself
    w.send(a, "USCH", fields![("USER", "bra"), ("MAXR", 10), ("ID", 3)]);
    assert_eq!(w.c(a).l("USCH").g("SIZE"), "1");
    assert_eq!(w.c(a).l("USER").g("USER"), "Bravo");
    w.send(a, "USCH", fields![("USER", "alp"), ("ID", 3)]);
    assert_eq!(w.c(a).l("USCH").g("SIZE"), "0");
    // Alpha adds Bravo: Bravo hears of it, Alpha sees Bravo's presence
    w.send(b, "PSET", fields![("SHOW", "CHAT"), ("STAT", "EX=0\nP=nfs5\n"), ("PROD", "is playing Underground 2")]);
    w.send(a, "RADD", fields![("LRSC", "PC"), ("USER", "Bravo"), ("LIST", "B"), ("ID", 5)]);
    assert_eq!(w.c(a).l("RADD").code, "");
    assert_eq!(w.c(a).l("PGET").g("USER"), "Bravo");
    assert_eq!(w.c(a).l("PGET").g("SHOW"), "CHAT");
    assert_eq!(w.c(b).l("RNOT").g("USER"), "Alpha");
    w.send(a, "RGET", fields![("LIST", "B"), ("ID", 2)]);
    assert_eq!(w.c(a).l("RGET").g("SIZE"), "1");
    assert_eq!(w.c(a).l("ROST").g("USER"), "Bravo");
    // presence changes reach the friend (at most once a second)
    w.advance(1000.0);
    w.send(b, "PSET", fields![("SHOW", "AWAY"), ("STAT", ""), ("PROD", "racing")]);
    assert_eq!(w.c(a).l("PGET").g("SHOW"), "AWAY");
    // a message, and a game invite passed on with the sender named by the server
    w.send(a, "SEND", fields![("USER", "Bravo"), ("BODY", "race?\u{7}"), ("SUBJ", ""), ("TYPE", "C")]);
    assert_eq!(w.c(b).l("RECV").g("USER"), "Alpha");
    assert_eq!(w.c(b).l("RECV").g("BODY"), "race?");
    w.send(a, "GINV", fields![("USER", "Bravo"), ("HOST", "Alpha"), ("TYPE", "G"), ("LKEY", "x")]);
    assert_eq!(w.c(b).l("GINV").g("USER"), "Alpha");
    assert_eq!(w.c(b).l("GINV").g("HOST"), "Alpha");
    assert_eq!(w.c(b).l("GINV").get("LKEY"), None);
    // a friend request from Bravo (to Alpha: one that waits for Alpha's answer), accepted by Alpha: both lists
    w.send(b, "RADM", fields![("LRSC", "PC"), ("USER", "Alpha"), ("ID", 102), ("PRES", "Y")]);
    assert_eq!(w.c(a).l("RNOT").g("USER"), "Bravo");
    assert_eq!(w.c(a).l("RNOT").g("ATTR"), "R");
    w.send(a, "RRSP", fields![("LRSC", "PC"), ("USER", "Bravo"), ("ANSW", "Y"), ("ID", 7)]);
    assert_eq!(w.c(b).l("RNOT").g("USER"), "Alpha");
    assert_eq!(w.c(b).l("RNOT").g("CHNG"), "A");
    assert!(w.core.buddies.list("Bravo").unwrap().b.contains_key("alpha"));
    let friends_lines = |w: &World| w.text().iter().filter(|l| l.contains("are friends")).count();
    assert_eq!(friends_lines(&w), 1);
    w.send(a, "RRSP", fields![("USER", "Bravo"), ("ANSW", "Y")]);
    assert_eq!(friends_lines(&w), 1, "an answer without a request does nothing");
    // crossing requests make friends too (the asked one uses "Send Friend Request" back)
    let gc = w.client("10.77.0.4");
    let lc = w.login(gc, "Charlie");
    let kc = w.lkey(lc);
    let c = w.buddy(gc, &kc);
    w.send(c, "RADM", fields![("USER", "Alpha"), ("ID", 1)]);
    assert_eq!(w.c(a).l("RNOT").g("USER"), "Charlie");
    assert_eq!(w.c(a).l("PGET").g("USER"), "Charlie");
    w.send(a, "RADM", fields![("USER", "Charlie"), ("ID", 2)]);
    assert_eq!(friends_lines(&w), 2);
    assert_eq!(w.c(c).l("RNOT").g("ATTR"), "0");
    assert!(
        w.core.buddies.list("Charlie").unwrap().b.contains_key("alpha") && w.core.buddies.list("Alpha").unwrap().b.contains_key("charlie")
    );
    // Bravo blocks Alpha: Alpha's messages no longer arrive
    w.send(b, "RADD", fields![("USER", "Alpha"), ("LIST", "I"), ("ID", 9)]);
    let before = w.c(b).count("RECV");
    w.send(a, "SEND", fields![("USER", "Bravo"), ("BODY", "hello?")]);
    assert_eq!(w.c(b).count("RECV"), before);
    // Bravo leaves: Alpha sees him offline; the friends list stays
    w.send(b, "DISC", vec![]);
    assert_eq!(w.c(a).l("PGET").g("SHOW"), "DISC");
    assert!(w.core.buddies.list("Alpha").unwrap().b.contains_key("bravo"));
    // an unknown key: refused once the lobby had a moment to finish
    let gx = w.client("10.77.0.9");
    let x = w.buddy(gx, "0123456789abcdef0123456789");
    w.sleep(3300.0);
    assert_eq!(w.c(x).l("AUTH").code, "pass");
    w.send(x, "RGET", fields![("LIST", "B")]);
    assert_eq!(w.c(x).l("RGET").code, "auth");
}

/// a player in Online mode with EA Messenger logged in: (lobby session, Messenger)
fn messenger(w: &mut World, vip: &str, name: &str) -> (C, C) {
    let g = w.client(vip);
    let l = w.login(g, name);
    let k = w.lkey(l);
    (l, w.buddy(g, &k))
}

/// the friends list as the game's login asks for it: (name, ATTR) of each ROST
fn roster(w: &mut World, b: C) -> Vec<(String, String)> {
    let n = w.c(b).msgs.len();
    w.send(b, "RGET", fields![("LRSC", "PC"), ("LIST", "B"), ("PRES", "Y"), ("PEND", "Y"), ("ID", 1)]);
    let rost = w.c(b).msgs[n..].iter().filter(|m| m.kind == "ROST");
    rost.map(|m| (m.g("USER").to_string(), m.get("ATTR").unwrap_or("").to_string())).collect()
}

fn told(w: &World, c: C, user: &str, chng: &str) -> usize {
    w.c(c).msgs.iter().filter(|m| m.kind == "RNOT" && m.get("USER") == Some(user) && m.get("CHNG") == Some(chng)).count()
}

// EA Messenger for players who are away: friend requests wait for their answer, over a restart too, and so do
// friends' messages; requests declined, blocked and taken back, a friend taken off; what waits too long goes
#[test]
fn ea_messenger_away() {
    let pair = |a: &str, b: &str| vec![(a.to_string(), b.to_string())];
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().join("online");
    let mut w = World::new(Opts { state_dir: Some(dir.clone()), ..Default::default() });
    let (_, a) = messenger(&mut w, "10.77.0.2", "Alpha");
    // a request to a player who was never here waits; the asker's list shows it as its own
    w.send(a, "RADM", fields![("LRSC", "PC"), ("USER", "Bravo"), ("ID", 5), ("PRES", "Y")]);
    assert_eq!(w.c(a).l("RADM").code, "");
    assert_eq!(roster(&mut w, a), pair("Bravo", "S"));
    // Bravo comes: on its list, a request that waits for its answer, with the asker's presence
    let (_, b) = messenger(&mut w, "10.77.0.3", "Bravo");
    assert_eq!(roster(&mut w, b), pair("Alpha", "R"));
    assert_eq!([w.c(b).l("PGET").g("USER"), w.c(b).l("PGET").g("SHOW")], ["Alpha", "CHAT"]);
    // Alpha leaves, then Bravo accepts: friends on both lists
    w.send(a, "DISC", vec![]);
    w.send(b, "RRSP", fields![("LRSC", "PC"), ("USER", "Alpha"), ("ANSW", "Y"), ("ID", 6), ("PRES", "Y")]);
    assert_eq!([w.c(b).l("RNOT").g("USER"), w.c(b).l("RNOT").g("ATTR")], ["Alpha", "0"]);
    assert!(w.core.buddies.list("Alpha").unwrap().b.contains_key("bravo"));
    // a message to Alpha, away: it waits, as Alpha has Bravo as a friend; a stranger's does not
    let sent = w.t.get();
    w.send(b, "SEND", fields![("USER", "Alpha"), ("BODY", "race me"), ("SUBJ", ""), ("TYPE", "C")]);
    assert_eq!(w.c(b).l("SEND").code, "");
    let (_, c) = messenger(&mut w, "10.77.0.4", "Charlie");
    w.send(c, "SEND", fields![("USER", "Alpha"), ("BODY", "spam")]);
    assert_eq!(w.core.buddies.list("Alpha").unwrap().m.len(), 1);
    // over a restart: Alpha's list has Bravo as a friend, then, a moment later (the game files it under the friend's
    // entry only then), the message comes, once
    w.core.stop();
    let mut w = World::new(Opts { state_dir: Some(dir.clone()), ..Default::default() });
    let (_, a) = messenger(&mut w, "10.77.0.2", "Alpha");
    assert_eq!(roster(&mut w, a), pair("Bravo", ""));
    assert_eq!(w.c(a).count("RECV"), 0, "not with the list");
    w.sleep(3000.0);
    let m = w.c(a).l("RECV");
    assert_eq!([m.g("USER"), m.g("BODY"), m.g("TYPE")], ["Bravo", "race me", "C"]);
    assert_eq!(m.g("TIME"), ((sent / 1000.0).floor() as i64).to_string());
    w.advance(1000.0);
    roster(&mut w, a);
    w.sleep(3000.0);
    assert_eq!(w.c(a).count("RECV"), 1, "a message comes once");
    // declined: Charlie's request goes, Charlie is told
    let (_, b) = messenger(&mut w, "10.77.0.3", "Bravo");
    let (_, c) = messenger(&mut w, "10.77.0.4", "Charlie");
    w.send(c, "RADM", fields![("USER", "Alpha"), ("ID", 1)]);
    assert_eq!([w.c(a).l("RNOT").g("USER"), w.c(a).l("RNOT").g("ATTR")], ["Charlie", "R"]);
    w.send(a, "RRSP", fields![("USER", "Charlie"), ("ANSW", "N"), ("ID", 2)]);
    assert_eq!(told(&w, c, "Alpha", "D"), 1);
    // blocked: Delta's request makes no friend, Delta is blocked, a request again never arrives
    let (_, d) = messenger(&mut w, "10.77.0.5", "Delta");
    w.send(d, "RADM", fields![("USER", "Alpha"), ("ID", 1)]);
    w.send(a, "RRSP", fields![("USER", "Delta"), ("ANSW", "B"), ("ID", 3)]);
    assert!(w.core.buddies.blocks("alpha", "delta"));
    assert!(!w.core.buddies.list("Alpha").unwrap().b.contains_key("delta"), "a request blocked makes no friend");
    w.send(d, "RADM", fields![("USER", "Alpha"), ("ID", 2)]);
    assert_eq!(told(&w, a, "Delta", "A"), 1, "Delta's second request");
    // taken back: Echo's request comes and goes
    let (_, e) = messenger(&mut w, "10.77.0.6", "Echo");
    w.send(e, "RADM", fields![("USER", "Alpha"), ("ID", 1)]);
    w.send(e, "RDEM", fields![("LRSC", "PC"), ("USER", "Alpha"), ("ID", 2)]);
    assert_eq!(told(&w, a, "Echo", "D"), 1);
    w.advance(1000.0);
    assert_eq!(roster(&mut w, a), pair("Bravo", ""), "no request of Charlie's, Delta's or Echo's");
    // a friend taken off (the game: RDEM, then RDEL with the same fields): off both lists, Bravo told once
    for (kind, id) in [("RDEM", 7), ("RDEL", -1)] {
        w.send(a, kind, fields![("LRSC", "PC"), ("LIST", "B"), ("PRES", "Y"), ("USER", "Bravo"), ("ID", id)]);
    }
    assert_eq!(told(&w, b, "Alpha", "D"), 1);
    assert!(w.core.buddies.list("Alpha").unwrap().b.is_empty() && w.core.buddies.list("Bravo").unwrap().b.is_empty());
    // what waits too long goes: a message after 7 days, a request after 30
    w.send(b, "RADM", fields![("USER", "Alpha"), ("ID", 3)]);
    w.send(a, "RRSP", fields![("USER", "Bravo"), ("ANSW", "Y"), ("ID", 4)]);
    w.send(e, "RADM", fields![("USER", "Alpha"), ("ID", 3)]);
    w.send(a, "DISC", vec![]);
    w.send(b, "SEND", fields![("USER", "Alpha"), ("BODY", "old news")]);
    assert_eq!(w.core.buddies.list("Alpha").unwrap().m.len(), 1);
    w.advance(8.0 * 86_400_000.0);
    let (_, a) = messenger(&mut w, "10.77.0.2", "Alpha");
    let rows = roster(&mut w, a);
    w.sleep(3000.0);
    assert_eq!(w.c(a).count("RECV"), 0, "a message 8 days old");
    assert!(rows.contains(&("Echo".to_string(), "R".to_string())), "a request 8 days old: {rows:?}");
    w.send(a, "DISC", vec![]);
    w.advance(23.0 * 86_400_000.0);
    let (_, a) = messenger(&mut w, "10.77.0.2", "Alpha");
    assert_eq!(roster(&mut w, a), pair("Bravo", ""), "a request 31 days old");
    w.core.buddy_save();
    let stored = std::fs::read_to_string(dir.join("buddies.json")).unwrap();
    assert!(!stored.contains("old news") && !stored.contains("\"R\""), "{stored}");
}

// the count of players in Online mode (the server's /players): logged in, or racing until back
#[test]
fn count_of_players() {
    let mut w = world();
    let (ga, gb) = (w.client("10.77.0.2"), w.client("10.77.0.3"));
    let a = w.login(ga, "Alpha");
    let b = w.login(gb, "Bravo");
    // and the browsers it counts (vips(): the server's console), the same
    let count = |w: &World| {
        let i = w.core.info();
        let v = w.core.vips();
        let (p, r) = (i["personas"].as_u64().unwrap(), i["racing"].as_u64().unwrap());
        assert_eq!(v.len() as u64, p + r);
        (p, r)
    };
    assert_eq!(count(&w), (2, 0));
    w.send(a, "gcre", fields![("NAME", "c1"), ("PARAMS", "TRACK=4014")]);
    w.send(b, "gjoi", fields![("NAME", "c1")]);
    w.send(a, "gsta", fields![("NAME", "c1")]);
    // the game closes its lobby link for the race
    w.close(a);
    w.close(b);
    assert_eq!(count(&w), (0, 2), "racing: still in Online mode");
    // Bravo's browser leaves during the race
    w.peer_gone(ip("10.77.0.3"));
    assert_eq!(count(&w), (0, 1));
    assert_eq!(w.core.vips().into_iter().collect::<Vec<_>>(), [ip("10.77.0.2")], "Alpha, racing");
    w.advance(200_000.0);
    let ga2 = w.client("10.77.0.2");
    // back online under the name: counted once
    w.login(ga2, "Alpha");
    assert_eq!(count(&w), (1, 0));
}
