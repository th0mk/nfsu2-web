//! NFS Most Wanted's Online title (title.rs MW): its redirector on 30920, the login and config it gets, the game's
//! four players (U-09 (a): a game asked for 8 takes 4, a gset cannot raise it, a fifth gjoi is refused), its rooms as
//! user sets (ucre, usea, ujoi, ulea, udel, uadm: lobby/usersets.rs, in the shapes EA's server.dll gave a Most Wanted
//! joiner in a LAN race), its chat's +msg flags, its race report and STAT field; and Underground 2's title untouched
//! by any of it (no user sets: badc, as before).

use super::world::{C, Opts, World};
use crate::lobby::{usersets_from_letters as from_letters, usersets_letters as letters};
use crate::stats::{Counts, Records, Table, decode_result_mw, stat_v44};
use crate::title::MW;
use crate::{PORT_LOBBY, PORT_REDIRECTOR};

fn world() -> World {
    World::new(Opts { title: Some(&MW), ..Opts::default() })
}

fn players(w: &mut World, names: &[&str]) -> Vec<C> {
    names
        .iter()
        .enumerate()
        .map(|(i, n)| {
            let g = w.client(&format!("10.77.0.{}", i + 2));
            w.login(g, n)
        })
        .collect()
}

// the redirector is on 30920 (the game's port word at 0x8F42E8), not Underground 2's 20920; @dir names the lobby
#[test]
fn redirector_on_30920() {
    let mut w = world();
    let g = w.client("10.77.0.2");
    let ug2 = w.connect(g, PORT_REDIRECTOR);
    assert!(w.c(ug2).reset, "no redirector on 20920 for Most Wanted");
    let c = w.connect(g, 30920);
    w.send_text(c, "@tic", "RC4+MD5-V2");
    assert_eq!(w.c(c).msgs.len(), 0, "no @tic answer: the link stays plain");
    w.send(c, "@dir", fields![("PROD", "nfs-pc-2006"), ("VERS", "pc/1.3-Nov 21 2005"), ("SKU", "14705")]);
    let d = w.c(c).l("@dir");
    assert_eq!((d.g("ADDR"), d.g("PORT")), ("10.77.0.1", PORT_LOBBY.to_string().as_str()));
    assert!(w.c(c).fin);
    let ssl = w.connect(g, 30921);
    assert!(w.c(ssl).reset, "no SSL port");
}

// the login: the config keys Most Wanted reads (the buddy server; none of Underground 2's), the rooms, the user sets
// the game looks at said back
#[test]
fn login_config_and_sele() {
    let mut w = world();
    let g = w.client("10.77.0.2");
    let c = w.login(g, "Razor");
    let news = w.c(c).l("news");
    assert_eq!(news.code, "new7");
    assert_eq!((news.g("BUDDY_SERVER"), news.g("BUDDY_PORT")), ("10.77.0.1", "13505"));
    for k in ["TOSURL", "NEWSURL", "GAMES_PER_UPDATE", "CIRCUIT_TIER_POINTS", "DRIFT_TIER_POINTS"] {
        assert!(news.get(k).is_none(), "{k}");
    }
    assert_eq!(w.c(c).count("+rom"), 4);
    assert_eq!(w.c(c).l("pers").g("PERS"), "Razor");
    w.send(c, "sele", fields![("MYGAME", 1), ("ROOMS", 1), ("USERSETS", 1), ("USERSET0", "093.Razor")]);
    let s = w.c(c).l("sele");
    assert_eq!((s.g("USERSETS"), s.g("USERSET0")), ("1", "093.Razor"));
}

// four players at most (U-09 (a)): a game asked for 8 takes 4, a gset cannot raise it, the fifth finds it full
#[test]
fn four_players() {
    let mut w = world();
    let cs = players(&mut w, &["Razor", "Bull", "Ming", "Vic", "Izzy"]);
    w.send(cs[0], "gcre", fields![("NAME", "Razor"), ("MAXSIZE", 8), ("MINSIZE", 2), ("SYSFLAGS", 0x40000)]);
    assert_eq!(w.c(cs[0]).l("gcre").g("MAXSIZE"), "4");
    w.send(cs[0], "gset", fields![("NAME", "Razor"), ("MAXSIZE", 8)]);
    assert_eq!(w.c(cs[0]).l("+mgm").g("MAXSIZE"), "4", "a gset keeps it at 4");
    for c in &cs[1..4] {
        w.send(*c, "gjoi", fields![("NAME", "Razor")]);
        assert_eq!(w.c(*c).l("gjoi").code, "");
    }
    w.send(cs[4], "gjoi", fields![("NAME", "Razor")]);
    assert_eq!(w.c(cs[4]).l("gjoi").code, "full");
    w.send(cs[0], "gsta", fields![]);
    let s = w.c(cs[3]).l("+ses");
    assert_eq!((s.g("COUNT"), s.g("OPPO3"), s.get("OPPO4")), ("4", "Vic", None));
}

// the flags as DirtySock writes them: a letter a bit, bit 0 '@' (server.dll's SF="KV" for ucre's SYSFLAGS 0x400800)
#[test]
fn flag_letters() {
    assert_eq!(letters(0), "");
    assert_eq!(letters(0x400800), "KV");
    assert_eq!(letters(0x8000_2C00), "JKM-");
    assert_eq!(letters(1), "@");
    assert_eq!(letters(0xF800_0000), "0123-");
    assert_eq!(from_letters("IJKMY-"), 0x8200_2E00);
    assert_eq!(from_letters("KV"), 0x400800);
    assert_eq!(from_letters("0123-@"), 0xF800_0001);
    assert_eq!((from_letters(""), from_letters("abc!")), (0, 0));
}

// a Quick Race in the shapes of EA's server: the creator makes a set and its game, a search finds the set by its
// custom flags, the joiner comes in (its record with IDENT, +who naming the set, +ust, a +usm for each member), the
// others hear of it, a fifth is refused, and the set goes with its creator
#[test]
fn user_sets_quick_race() {
    let mut w = world();
    let cs = players(&mut w, &["Razor", "Bull", "Ming", "Vic", "Izzy"]);
    let (host, join) = (cs[0], cs[1]);
    // (a Most Wanted host's: its flags as letters, SYSFLAGS 0x400800 and CUSTFLAGS 0x82002E00)
    w.send(
        host,
        "ucre",
        fields![
            ("NAME", "093.Razor"),
            ("SIZE", 8),
            ("TYPE", 0),
            ("UPDATES", 0),
            ("SYSFLAGS", "KV"),
            ("CUSTFLAGS", "IJKMY-"),
            ("PARAMS", "V=1\nP=2")
        ],
    );
    let r = w.c(host).l("ucre");
    assert_eq!(r.code, "");
    assert_eq!((r.g("N"), r.g("O"), r.g("S"), r.g("SF"), r.g("CF"), r.g("C")), ("093.Razor", "Razor", "4", "KV", "IJKMY-", "1"));
    let set_id = r.g("I").to_string();
    assert_eq!(w.c(host).l("+who").g("US"), "093.Razor");
    w.send(host, "gcre", fields![("NAME", "Razor"), ("MAXSIZE", 4), ("MINSIZE", 2)]);
    // the joiner's search: the game's own flags and mask, as numbers (an Online sprint's: 0x80002600 under 0x80006001)
    w.cm(join).take();
    w.send(join, "usea", fields![("START", 0), ("COUNT", 100), ("CUSTFLAGS", -2147473920), ("CUSTMASK", -2147457535)]);
    assert_eq!(w.c(join).l("usea").g("COUNT"), "1");
    assert_eq!(w.c(join).l("+uss").g("N"), "093.Razor");
    w.send(join, "usea", fields![("START", 0), ("COUNT", 100), ("CUSTFLAGS", 1), ("CUSTMASK", 1)]);
    assert_eq!(w.c(join).l("usea").g("COUNT"), "0", "other flags: none");
    w.cm(join).take();
    w.cm(host).take();
    w.send(join, "ujoi", fields![("NAME", "093.Razor")]);
    let j = w.c(join).l("ujoi");
    assert_eq!((j.code.as_str(), j.g("C"), j.g("IDENT")), ("", "2", set_id.as_str()));
    assert_eq!(w.c(join).l("+who").g("US"), "093.Razor");
    assert_eq!(w.c(join).count("+usm"), 2, "a record for each member");
    assert_eq!((w.c(host).l("+ust").g("C"), w.c(host).l("+usm").g("N")), ("2", "Bull"));
    w.send(join, "gjoi", fields![("NAME", "Razor")]);
    assert_eq!(w.c(join).l("gjoi").g("HOST"), "Razor");
    assert_eq!(w.c(host).l("+usm").g("G"), "1", "the members see the joiner in the game");
    // the car description goes to the set's members
    w.send(join, "auxi", fields![("TEXT", "SCF=0\nCN=2048151865\nV=20\n")]);
    assert!(w.c(host).l("+usm").g("X").contains("CN=2048151865"));
    for c in &cs[2..4] {
        w.send(*c, "ujoi", fields![("NAME", "093.Razor")]);
        assert_eq!(w.c(*c).l("ujoi").code, "");
    }
    w.send(cs[4], "ujoi", fields![("NAME", "093.razor")]);
    assert_eq!(w.c(cs[4]).l("ujoi").code, "full", "four at most, the name in any case");
    // the creator's admin: only the creator
    w.send(join, "uadm", fields![("NAME", "093.Razor"), ("PARAMS", "x")]);
    assert_eq!(w.c(join).l("uadm").code, "host");
    w.send(host, "uadm", fields![("NAME", "093.Razor"), ("DESC", "$02004d0a"), ("PARAMS", "V=2")]);
    assert_eq!(w.c(join).l("+ust").g("D"), "$02004d0a");
    // a member leaves: the others hear of it; the creator leaves: the set goes
    w.send(cs[3], "ulea", fields![("NAME", "093.Razor")]);
    assert_eq!(w.c(cs[3]).l("+who").g("US"), "");
    assert_eq!(w.c(join).l("+ust").g("C"), "3");
    w.send(host, "ulea", fields![]);
    assert_eq!(w.c(join).l("+who").g("US"), "", "the set is gone");
    w.send(cs[4], "usea", fields![("START", 0), ("COUNT", 100), ("CUSTFLAGS", 0), ("CUSTMASK", 0)]);
    assert_eq!(w.c(cs[4]).l("usea").g("COUNT"), "0");
    assert!(w.text().iter().any(|l| l.contains("the room \"093.Razor\" ended: its creator left")));
}

// a set's name once; a joiner leaving the lobby leaves its set; udel by the creator only
#[test]
fn user_sets_names_and_leaving() {
    let mut w = world();
    let cs = players(&mut w, &["Razor", "Bull", "Ming"]);
    w.send(cs[0], "ucre", fields![("NAME", "1.Razor"), ("SIZE", 4)]);
    w.send(cs[1], "ucre", fields![("NAME", "1.RAZOR"), ("SIZE", 4)]);
    assert_eq!(w.c(cs[1]).l("ucre").code, "dupl");
    w.send(cs[1], "ucre", fields![("NAME", ""), ("SIZE", 4)]);
    assert_eq!(w.c(cs[1]).l("ucre").code, "inam");
    w.send(cs[1], "ujoi", fields![("NAME", "nope")]);
    assert_eq!(w.c(cs[1]).l("ujoi").code, "unkn");
    w.send(cs[1], "ujoi", fields![("NAME", "1.Razor")]);
    w.send(cs[2], "ujoi", fields![("NAME", "1.Razor")]);
    w.send(cs[1], "udel", fields![("NAME", "1.Razor")]);
    assert_eq!(w.c(cs[1]).l("udel").code, "host");
    w.rst(cs[2]);
    assert_eq!(w.c(cs[0]).l("+ust").g("C"), "2", "a member who left the lobby is out");
    w.send(cs[0], "udel", fields![("NAME", "1.Razor")]);
    assert_eq!(w.c(cs[0]).l("udel").code, "");
    assert!(w.core.lobby.sets.is_empty());
    assert_eq!(w.c(cs[1]).l("+who").g("US"), "");
}

// the game room's chat: F the ATTR's letters with U (EGS: EGSU), the countdown's as it is (EGT), U empty; the reply
// carries nothing
#[test]
fn chat_flags() {
    let mut w = world();
    let cs = players(&mut w, &["Razor", "Bull"]);
    w.send(cs[0], "gcre", fields![("NAME", "Razor"), ("MAXSIZE", 4)]);
    w.send(cs[1], "gjoi", fields![("NAME", "Razor")]);
    w.send(cs[1], "mesg", fields![("TEXT", "42"), ("ATTR", "EGS")]);
    assert!(w.c(cs[1]).l("mesg").get("ATTR").is_none());
    let m = w.c(cs[0]).l("+msg");
    assert_eq!((m.g("F"), m.g("T"), m.g("U"), m.g("N")), ("EGSU", "42", "", "Bull"));
    w.send(cs[0], "mesg", fields![("TEXT", "TIME=-1\nPAUSE=0\nHURRY=0"), ("ATTR", "EGT")]);
    assert_eq!(w.c(cs[1]).l("+msg").g("F"), "EGT");
}

// Underground 2's title has no user sets: their messages are badc, as before; its +msg as before
#[test]
fn ug2_has_no_user_sets() {
    let mut w = World::new(Opts::default());
    let g = w.client("10.77.0.2");
    let c = w.login(g, "Speedy");
    for k in ["ucre", "usea", "ujoi", "ulea", "udel", "uadm"] {
        w.send(c, k, fields![("NAME", "x")]);
        assert_eq!(w.c(c).l(k).code, "badc", "{k}");
    }
    w.send(c, "sele", fields![("USERSETS", 1), ("USERSET0", "x")]);
    let s = w.c(c).l("sele");
    assert_eq!((s.g("USERSETS"), s.get("USERSET0")), ("0", None));
    assert_eq!(w.c(c).l("+who").g("US"), "");
}

// the race report as speed.exe writes it (0x741780): 8 bytes, then 51 a player and 2 for each counter in its mask
#[test]
fn race_report() {
    let mut b = vec![3u8, 2, 1, 0, 0x07, 0xD0, 0, 1];
    let player = |idx: u8, place: u8, reason: u8, time: f32, top: f32, mask: u16| {
        let mut p = vec![0u8; 51];
        p[0] = idx;
        p[1] = place;
        p[2] = reason;
        p[8..12].copy_from_slice(&time.to_be_bytes());
        p[20..24].copy_from_slice(&top.to_be_bytes());
        p[49..51].copy_from_slice(&mask.to_be_bytes());
        p
    };
    b.extend(player(0, 1, 1, 95.5, 61.0, 0b101));
    b.extend([0, 7, 0, 9]);
    b.extend(player(1, 2, 0x0c, 40.0, 50.0, 0));
    let s = base64(&b);
    let r = decode_result_mw(&s).expect("parses");
    assert_eq!((r.kind, r.reporter, r.track, r.laps, r.players.len()), (1, 0, 2000, 1, 2));
    assert_eq!((r.players[0].place, r.players[0].time, r.players[0].top, r.players[0].dropped), (1, 95.5, 61.0, false));
    assert_eq!((r.players[1].index, r.players[1].place, r.players[1].dropped), (1, 2, true), "after the first's counters");
    // more than 4 players, a cut report, another version: none
    let mut five = b.clone();
    five[1] = 5;
    assert!(decode_result_mw(&base64(&five)).is_none());
    assert!(decode_result_mw(&base64(&b[..b.len() - 1])).is_none());
    let mut v2 = b.clone();
    v2[0] = 2;
    assert!(decode_result_mw(&base64(&v2)).is_none());
}

// the STAT field: 44 numbers, the disconnects at 1, a block of 12 per race type at 8 + 12t
#[test]
fn stat_field() {
    let mut t: Table = [None; 6];
    t[1] = Some(Counts { races: 3.0, rw: 2.0, rl: 1.0, uw: 0.0, ul: 0.0, points: 150.0, dc: 1.0 });
    let s = stat_v44(&t, &Records { top: 70.5, ..Records::default() });
    let v: Vec<&str> = s.trim_end_matches(',').split(',').collect();
    assert_eq!(v.len(), 44);
    assert_eq!(v[1], "1");
    assert_eq!(&v[20..26], ["96", "2", "1", "0", "0", "11364"]);
    assert_eq!(v[8], "0");
}

fn base64(b: &[u8]) -> String {
    const A: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::new();
    for c in b.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 {
            if i <= c.len() {
                s.push(A[(n >> (18 - 6 * i) & 63) as usize] as char);
            } else {
                s.push('=');
            }
        }
    }
    s
}
