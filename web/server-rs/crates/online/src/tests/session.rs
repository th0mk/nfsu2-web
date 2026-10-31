//! Scripted random sessions of the whole online host (a fixed seed each): six browsers (two of them one client) log
//! in, chat, make and join and start games, report races, use EA Messenger, go and come back, fall behind (the switch
//! refuses their frames) and leave; the lobby's clock moves.  After every step the lobby's counts agree with its
//! sessions (`Lobby::check_counts`), nothing panics, and the files the host writes at its stop read back to the same
//! bytes.  The script's LKEY and AUTH values are the ones the host gave (`{"lkey": conn}`, `{"ses": [conn, key]}`).

use serde_json::{Value, json};

use super::world::{C, Opts, World, ip};
use crate::aries::{Fields, V};
use crate::{PORT_BUDDY, PORT_HTTP, PORT_LOBBY, PORT_REDIRECTOR};

const T0: f64 = 1_792_411_200_000.0; // Date.UTC(2026, 9, 19, 12)

/// xorshift64*: the same session for the same seed
struct Rng(u64);

impl Rng {
    fn new(seed: u64) -> Rng {
        Rng(seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1)
    }
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }
    fn below(&mut self, n: usize) -> usize {
        (self.next() % n as u64) as usize
    }
    fn pick<'a, T>(&mut self, xs: &'a [T]) -> &'a T {
        &xs[self.below(xs.len())]
    }
    fn chance(&mut self, p: f64) -> bool {
        (self.next() >> 11) as f64 / (1u64 << 53) as f64 <= p
    }
}

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
}

/// a latin1 string as the script keeps one (hex of its bytes)
fn hs(s: &str) -> String {
    let mut b = Vec::new();
    crate::jsx::latin1_encode(s, &mut b);
    crate::jsx::hex(&b)
}

/// a race report for n players, the places and times from the seed
fn resu(n: usize, seed: u64) -> String {
    let mut b = vec![0u8; 8 + 55 * n];
    b[0] = 3;
    b[1] = n as u8;
    b[2] = (seed % 7) as u8;
    b[4..6].copy_from_slice(&4014u16.to_be_bytes());
    b[7] = 2;
    for i in 0..n {
        let o = 8 + 55 * i;
        b[o] = i as u8;
        b[o + 1] = ((i as u64 + seed) % n as u64) as u8 + 1;
        b[o + 2] = if (seed + i as u64).is_multiple_of(5) { 0x0c } else { 1 };
        let t = (25.0 + i as f64 * 1.5 + (seed % 7) as f64 * 10.25) as f32;
        b[o + 8..o + 12].copy_from_slice(&t.to_be_bytes());
    }
    use base64::Engine;
    base64::engine::general_purpose::STANDARD.encode(b)
}

pub struct Session {
    pub steps: Vec<Value>,
}

struct P {
    vip: &'static str,
    group: &'static str,
    name: &'static str,
    lobby: Option<usize>,
    buddy: Option<usize>,
}

struct Gen {
    r: Rng,
    steps: Vec<Value>,
    conns: usize,
    port: u16,
    ps: Vec<P>,
    races: Vec<(Vec<usize>, Vec<usize>, u64)>,
    games: usize,
    /// odd frames and the timers too (`generate_ext`)
    ext: bool,
}

const NAMES: [&str; 6] = ["Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot"];
const TEXTS: [&str; 10] = [
    "hello",
    "two words",
    "\u{7}bell",
    "caf\u{e9} \u{ff}",
    "",
    "   ",
    "x=y \"q\" %20",
    "a\tb\nc",
    "\u{9b}\u{ad}soft",
    "long long long long long long text",
];

fn s(x: &str) -> Value {
    json!(hs(x))
}

impl Gen {
    fn step(&mut self, v: Value) {
        self.steps.push(v);
    }
    fn connect(&mut self, p: usize, dport: u16) -> usize {
        let i = self.conns;
        self.conns += 1;
        self.port += 1;
        let (vip, group) = (self.ps[p].vip, self.ps[p].group);
        self.step(json!({ "op": "connect", "vip": vip, "group": group, "port": self.port, "dport": dport, "cid": 7000 + i }));
        i
    }
    fn send(&mut self, c: usize, kind: &str, fields: Vec<(&str, Value)>) {
        let f: Vec<Value> = fields.into_iter().map(|(k, v)| json!([hs(k), v])).collect();
        self.step(json!({ "op": "send", "conn": c, "kind": hs(kind), "fields": f }));
    }
    fn login(&mut self, p: usize, persona: &str) {
        let c = self.connect(p, PORT_LOBBY);
        self.send(c, "addr", vec![("ADDR", s("10.77.0.99")), ("PORT", json!(50000 + c))]);
        self.send(c, "skey", vec![("SKEY", s("$5075626c6963204b6579"))]);
        self.send(c, "news", vec![("NAME", json!(7))]);
        self.send(c, "sele", vec![("MYGAME", json!(1)), ("STATS", json!(5000))]);
        let name = self.ps[p].name;
        self.send(c, "auth", vec![("NAME", s(name)), ("PASS", s("secret")), ("REGKEY", s("AAAA"))]);
        if persona != name {
            self.send(c, "cper", vec![("PERS", s(persona))]);
        }
        self.send(c, "pers", vec![("PERS", s(persona))]);
        self.ps[p].lobby = Some(c);
    }
    fn buddy(&mut self, p: usize) {
        let Some(l) = self.ps[p].lobby else { return };
        let c = self.connect(p, PORT_BUDDY);
        self.send(c, "AUTH", vec![("PROD", s("NFS-CONSOLE-2005")), ("LKEY", json!({ "lkey": l }))]);
        self.ps[p].buddy = Some(c);
    }
    fn some_name(&mut self) -> String {
        let n = *self.r.pick(&NAMES);
        match self.r.below(5) {
            0 => n.to_lowercase(),
            1 => format!("{n}@ea.com"),
            2 => "Nobody".into(),
            _ => n.into(),
        }
    }
    fn maybe(&mut self, f: &mut Vec<(&'static str, Value)>, k: &'static str, pool: &[Value]) {
        if self.r.chance(0.6) {
            f.push((k, self.r.pick(pool).clone()));
        }
    }

    fn lobby_message(&mut self, p: usize) {
        let Some(c) = self.ps[p].lobby else { return };
        let mut f: Vec<(&'static str, Value)> = Vec::new();
        let games = ["Race1", "Race2", "Race 3", "\u{1}"];
        let kind = *self.r.pick(&[
            "mesg", "mesg", "move", "move", "auxi", "gsea", "gsea", "gcre", "gjoi", "gjoi", "gset", "gset", "gsta", "gdel", "glea", "user",
            "onln", "snap", "snap", "rept", "lost", "sele", "news", "auth", "cper", "dper", "pers", "zzzz", "~png", "skey",
        ]);
        match kind {
            "mesg" => {
                f.push(("TEXT", s(self.r.pick(&TEXTS))));
                if self.r.chance(0.3) {
                    let n = self.some_name();
                    f.push(("PRIV", s(&n)));
                }
                self.maybe(&mut f, "ATTR", &[s(""), s("G"), s("X"), s("GGGGGGGGGGGG")]);
            }
            "move" => f.push(("NAME", s(self.r.pick(&["Lounge", "Circuit", "sprint", "URL", "nowhere", ""])))),
            "auxi" => f.push(("TEXT", s(self.r.pick(&TEXTS)))),
            "gsea" => {
                self.maybe(&mut f, "START", &[json!(0), json!(1), json!(-1), s("x")]);
                self.maybe(&mut f, "COUNT", &[json!(0), json!(2), json!(20), json!(99)]);
                self.maybe(&mut f, "CUSTFLAGS", &[json!(0), json!(0x40000f3), json!(0x100)]);
                self.maybe(&mut f, "CUSTMASK", &[json!(0), json!(0x40001f3), json!(-1)]);
                self.maybe(&mut f, "SYSFLAGS", &[json!(0), json!(1 << 18)]);
                self.maybe(&mut f, "SYSMASK", &[json!(0), json!(0xc0000)]);
            }
            "gcre" => {
                f.push(("NAME", s(self.r.pick(&games))));
                self.maybe(&mut f, "MAXSIZE", &[json!(0), json!(2), json!(4), json!(9), s("x")]);
                self.maybe(&mut f, "MINSIZE", &[json!(0), json!(3), json!(7)]);
                self.maybe(&mut f, "CUSTFLAGS", &[json!(0), json!(0x40000f3), json!(0x100), json!(4294967295u32)]);
                self.maybe(&mut f, "SYSFLAGS", &[json!(0), json!(1 << 18), json!((1 << 16) | (1 << 19)), json!(-1)]);
                self.maybe(&mut f, "PARAMS", &[s("TRACK=4014"), s("TRACK=1\nLAPS=2"), s(&"P".repeat(300))]);
            }
            "gjoi" => {
                if self.r.chance(0.7) {
                    f.push(("NAME", s(self.r.pick(&games))));
                } else {
                    f.push(("IDENT", self.r.pick(&[json!(1), json!(2), json!(3), s("x"), s("1.0")]).clone()));
                }
            }
            "gset" => {
                self.maybe(&mut f, "USERFLAGS", &[json!(0), json!(1 << 27), s("x"), json!(-1)]);
                self.maybe(&mut f, "PARAMS", &[s("TRACK=2"), s("TRACK=4014")]);
                self.maybe(&mut f, "MAXSIZE", &[json!(2), json!(6), json!(1)]);
                self.maybe(&mut f, "MINSIZE", &[json!(2), json!(5)]);
                self.maybe(&mut f, "SYSFLAGS", &[json!(0), json!(1 << 12), json!(4294967295u32)]);
                if self.r.chance(0.3) {
                    let k = format!("{}, {} ,{}", self.some_name(), self.some_name(), self.some_name());
                    f.push(("KICK", s(&k)));
                }
            }
            "user" | "onln" | "rept" => {
                let n = self.some_name();
                f.push(("PERS", s(&n)));
            }
            "snap" => {
                f.push(("INDEX", json!(self.r.below(9))));
                f.push((
                    "CHAN",
                    self.r
                        .pick(&[
                            json!(0),
                            json!(6),
                            json!(7),
                            json!(11),
                            json!(12),
                            json!(13),
                            json!(18),
                            json!(24),
                            json!(29),
                            json!(30),
                            s("x"),
                        ])
                        .clone(),
                ));
                self.maybe(&mut f, "START", &[json!(0), json!(1), json!(-1)]);
                self.maybe(&mut f, "RANGE", &[json!(0), json!(1), json!(10), json!(200)]);
                if self.r.chance(0.3) {
                    let find = self.r.pick(&["$", "$Alpha", "bravo", ""]).to_string();
                    f.push(("FIND", s(&find)));
                }
            }
            "sele" => self.maybe(&mut f, "STATS", &[json!(5000), s("x"), json!(99999), json!(-3)]),
            "news" => f.push(("NAME", json!(*self.r.pick(&[6, 7])))),
            "auth" => f.push(("NAME", s(self.ps[p].name))),
            "cper" | "dper" | "pers" => {
                let x = self.r.pick(&["Second", "Third", "x y", "Sp1cs", self.ps[p].name]).to_string();
                f.push(("PERS", s(&x)));
            }
            _ => {}
        }
        self.send(c, kind, f);
    }

    fn buddy_message(&mut self, p: usize) {
        let Some(c) = self.ps[p].buddy else { return };
        let kind = *self.r.pick(&[
            "RGET", "RADD", "RADD", "RDEL", "RADM", "RADM", "RRSP", "PADD", "PDEL", "USCH", "PSET", "PSET", "SEND", "GINV", "GRSP", "EPGT",
            "PING", "DISC", "AUTH", "ZZZZ",
        ]);
        let mut f: Vec<(&'static str, Value)> = Vec::new();
        self.maybe(&mut f, "ID", &[json!(1), json!(7), s("x")]);
        match kind {
            "RGET" => self.maybe(&mut f, "LIST", &[s("B"), s("I"), s("i"), s("x")]),
            "RADD" | "RDEL" | "RADM" | "PADD" | "PDEL" => {
                let n = self.some_name();
                f.push(("USER", s(&n)));
                self.maybe(&mut f, "LIST", &[s("B"), s("I")]);
            }
            "RRSP" => {
                let n = self.some_name();
                f.push(("USER", s(&n)));
                self.maybe(&mut f, "ANSW", &[s("Y"), s("N"), s("no"), s("FALSE"), s("0"), s("yes"), s("")]);
            }
            "USCH" => {
                f.push(("USER", s(self.r.pick(&["a", "b", "al", "zz", "", "CH", "d@x"]))));
                self.maybe(&mut f, "MAXR", &[json!(0), json!(1), json!(5), json!(30)]);
            }
            "PSET" => {
                self.maybe(&mut f, "SHOW", &[s("CHAT"), s("AWAY"), s("XA-VERY-LONG")]);
                self.maybe(&mut f, "STAT", &[s(""), s("EX=0\nP=nfs5\n")]);
                self.maybe(&mut f, "PROD", &[s("racing"), s("in the lobby"), s("\u{7}x")]);
            }
            "SEND" | "GINV" | "GRSP" => {
                let n = self.some_name();
                f.push(("USER", s(&n)));
                f.push(("BODY", s(self.r.pick(&TEXTS))));
                self.maybe(&mut f, "SUBJ", &[s("hi"), s("")]);
                self.maybe(&mut f, "TYPE", &[s("C"), s("GAMEINVITE")]);
                self.maybe(&mut f, "LKEY", &[s("x")]);
                if kind != "SEND" && self.r.chance(0.2) {
                    // a field name the server cannot pass on (its own encoder refuses it): the body as text
                    f.push(("A-B", s("bad key")));
                    let mut text = Vec::new();
                    for (k, v) in &f {
                        text.extend_from_slice(k.as_bytes());
                        text.push(b'=');
                        let v = match v {
                            Value::String(x) => V::S(latin1(&json!(x))),
                            Value::Number(n) => V::N(n.as_f64().unwrap()),
                            _ => unreachable!(),
                        };
                        crate::aries::encode_value(&v, &mut text);
                        text.push(b'\n');
                    }
                    let t = crate::jsx::latin1_decode(&text);
                    self.step(json!({ "op": "text", "conn": c, "kind": hs(kind), "text": hs(&t) }));
                    return;
                }
            }
            "AUTH" => f.push(("LKEY", json!({ "lkey": self.ps[p].lobby.unwrap_or(0) }))),
            _ => {}
        }
        self.send(c, kind, f);
    }

    fn race(&mut self) {
        let mut who: Vec<usize> = (0..self.ps.len()).filter(|p| self.ps[*p].lobby.is_some()).collect();
        if who.len() < 2 {
            return;
        }
        let n = 2 + self.r.below(2.min(who.len() - 1));
        let mut ps = Vec::new();
        for _ in 0..n {
            ps.push(who.remove(self.r.below(who.len())));
        }
        self.games += 1;
        let name = format!("R{}", self.games);
        let conns: Vec<usize> = ps.iter().map(|p| self.ps[*p].lobby.unwrap()).collect();
        self.send(conns[0], "gcre", vec![("NAME", s(&name)), ("PARAMS", s("TRACK=4014"))]);
        for c in &conns[1..] {
            self.send(*c, "gjoi", vec![("NAME", s(&name))]);
        }
        self.send(conns[0], "gsta", vec![("NAME", s(&name))]);
        let seed = self.r.next() % 1000;
        self.races.push((ps, conns, seed));
    }

    /// a friend request between two players with Messenger, answered (yes, no, or a request back)
    fn friends(&mut self) {
        let who: Vec<usize> = (0..self.ps.len()).filter(|p| self.ps[*p].buddy.is_some()).collect();
        if who.len() < 2 {
            return;
        }
        let a = *self.r.pick(&who);
        let b = *self.r.pick(&who);
        let (ca, cb) = (self.ps[a].buddy.unwrap(), self.ps[b].buddy.unwrap());
        let (na, nb) = (self.ps[a].name, self.ps[b].name);
        if self.r.chance(0.3) {
            self.send(ca, "RADD", vec![("USER", s(nb)), ("LIST", s("B"))]);
        }
        self.send(ca, "RADM", vec![("USER", s(nb)), ("ID", json!(3))]);
        match self.r.below(3) {
            0 => self.send(cb, "RADM", vec![("USER", s(na))]),
            1 => self.send(cb, "RRSP", vec![("USER", s(na)), ("ANSW", s("N"))]),
            _ => self.send(cb, "RRSP", vec![("USER", s(&na.to_lowercase())), ("ANSW", s("Y")), ("ID", json!(4))]),
        }
        if self.r.chance(0.5) {
            self.send(cb, "RGET", vec![("LIST", s("B"))]);
        }
    }

    /// a game whose creator kicks some players (the countdown's end), then starts or deletes it
    fn kick(&mut self) {
        let mut who: Vec<usize> = (0..self.ps.len()).filter(|p| self.ps[*p].lobby.is_some()).collect();
        if who.len() < 3 {
            return;
        }
        let mut ps = Vec::new();
        for _ in 0..3 {
            ps.push(who.remove(self.r.below(who.len())));
        }
        self.games += 1;
        let name = format!("K{}", self.games);
        let conns: Vec<usize> = ps.iter().map(|p| self.ps[*p].lobby.unwrap()).collect();
        self.send(conns[0], "gcre", vec![("NAME", s(&name)), ("MAXSIZE", json!(4)), ("PARAMS", s("TRACK=4011"))]);
        for c in &conns[1..] {
            self.send(*c, "gjoi", vec![("NAME", s(&name))]);
        }
        self.send(conns[1], "gset", vec![("USERFLAGS", json!(1 << 27))]);
        self.send(conns[0], "gset", vec![("SYSFLAGS", json!(1 << 12)), ("USERFLAGS", json!(1 << 27))]);
        let kicked = format!(" {} ,{}", self.ps[ps[2]].name.to_uppercase(), self.ps[ps[0]].name);
        self.send(conns[0], "gset", vec![("KICK", s(&kicked))]);
        let last = if self.r.chance(0.5) { "gsta" } else { "gdel" };
        self.send(conns[0], last, vec![]);
    }

    fn report(&mut self) {
        if self.races.is_empty() {
            return;
        }
        let (ps, conns, seed) = self.races.remove(self.r.below(self.races.len()));
        for (p, old) in ps.iter().zip(&conns) {
            if self.r.chance(0.8) {
                let name = self.ps[*p].name;
                if let Some(l) = self.ps[*p].lobby {
                    self.step(json!({ "op": "fin", "conn": l }));
                }
                self.login(*p, name);
            }
            let Some(l) = self.ps[*p].lobby else { continue };
            let agree = if self.r.chance(0.85) { seed } else { seed + 1 };
            let f = vec![
                json!([hs("AUTH"), { "ses": [old, "AUTH"] }]),
                json!([hs("RESU"), { "resu": [old, agree] }]),
                json!([hs("VENUE"), hs("NFSUG2")]),
            ];
            self.step(json!({ "op": "send", "conn": l, "kind": hs("rank"), "fields": f, "names": old }));
        }
    }

    /// a whole switch frame from player p's browser: short ones, other kinds and ops, streams that are not there
    fn odd_frame(&mut self, p: usize) {
        use crate::vhost::lan_frame;
        let (vip, group) = (ip(self.ps[p].vip), self.ps[p].group);
        let host = crate::VHOST_VIP;
        let port = *self.r.pick(&[PORT_LOBBY, PORT_BUDDY, PORT_HTTP, PORT_REDIRECTOR, 9]);
        // a stream of this player's (its port and id), or one that is not
        let (sport, cid) = match self.ps[p].lobby.filter(|_| self.r.chance(0.5)) {
            Some(c) => (40000 + c as u16 + 1, 7000 + c as u32),
            None => (39999, 1),
        };
        let kind = self.r.below(10);
        let f = match kind {
            0 => unhex(self.r.pick(&["", "04", "040000000a4d0002", "04000000000000000000000000000000000000"])),
            1 => lan_frame(4, vip, host, sport, port, *self.r.pick(&[0, 2, 6, 255]), cid, b"x"),
            2 => lan_frame(4, vip, host, sport, port, crate::vhost::SYNACK, cid, &[]),
            3 => lan_frame(4, vip, host, sport, port, crate::vhost::DATA, cid, &[]),
            4 => lan_frame(4, vip, host, sport, port, crate::vhost::FIN, cid, &[]),
            5 => lan_frame(*self.r.pick(&[0, 1, 2, 6, 9]), vip, host, sport, port, 1, cid, b"abc"),
            6 => {
                let echo = unhex(self.r.pick(&["0800aabb00010002", "08", "0000aabb00010002", "0800000000000000ff"]));
                lan_frame(5, vip, host, 0, 0, 0, 0, &echo)
            }
            7 => lan_frame(3, vip, host, 3658, *self.r.pick(&[3658, 53]), *self.r.pick(&[0, 1]), 0, b"udp"),
            // a duplicate SYN of a stream that is there
            8 => lan_frame(4, vip, host, sport, PORT_LOBBY, crate::vhost::SYN, cid, &[]),
            // the middle of a message, then nothing: its stream resets
            _ => {
                let part = &crate::aries::encode_frame("mesg", "", &fields![("TEXT", "cut short")]).unwrap()[..15];
                lan_frame(4, vip, host, sport, PORT_LOBBY, crate::vhost::DATA, cid, part)
            }
        };
        self.step(json!({ "op": "frame", "vip": self.ps[p].vip, "group": group, "hex": crate::jsx::hex(&f) }));
        // (a stream left in the middle of a message always resets: what came after it would be read from the middle
        // of a message, and each side's own random values would be taken for its length)
        if kind == 9 || self.r.chance(0.3) {
            let rst = lan_frame(4, vip, host, sport, PORT_LOBBY, crate::vhost::RST, cid, &[]);
            self.step(json!({ "op": "frame", "vip": self.ps[p].vip, "group": group, "hex": crate::jsx::hex(&rst) }));
        }
    }

    /// what the timers do: streams that never say anything (the redirector's, the web pages', a Messenger that never
    /// logs in), a Messenger login before its lobby login is done (tried again), presence held back, a second lobby
    /// session of one browser, then time passes
    fn timed(&mut self, p: usize) {
        match self.r.below(6) {
            0 => {
                let port = *self.r.pick(&[PORT_REDIRECTOR, PORT_HTTP, PORT_BUDDY]);
                self.connect(p, port);
            }
            1 => {
                let c = self.connect(p, PORT_BUDDY);
                let key = self.r.pick(&["", "0123456789abcdef0123456789abcdef", "short"]).to_string();
                self.send(c, "AUTH", vec![("LKEY", s(&key))]);
            }
            2 => {
                if let Some(b) = self.ps[p].buddy {
                    for prod in ["a", "b", "c"] {
                        self.send(b, "PSET", vec![("SHOW", s("CHAT")), ("PROD", s(prod))]);
                    }
                }
            }
            3 => {
                // a reconnect that overlaps the old session (the old one is not closed first)
                let name = self.ps[p].name;
                self.login(p, name);
            }
            _ => {}
        }
        let ms = *self.r.pick(&[1.0, 300.0, 499.0, 500.0, 999.0, 1000.0, 1001.0, 3000.0, 5000.0, 10_000.0, 15_000.0, 30_001.0, 61_000.0]);
        self.step(json!({ "op": "sleep", "ms": ms }));
    }

    fn action(&mut self) {
        let p = self.r.below(self.ps.len());
        if self.ext && self.r.chance(0.25) {
            return if self.r.chance(0.5) { self.odd_frame(p) } else { self.timed(p) };
        }
        match self.r.below(100) {
            0..=29 => self.lobby_message(p),
            30..=54 => self.buddy_message(p),
            55..=57 => {
                if let Some(l) = self.ps[p].lobby {
                    self.step(json!({ "op": "fin", "conn": l }));
                }
                let persona = if self.r.chance(0.2) { self.r.pick(&NAMES).to_string() } else { self.ps[p].name.to_string() };
                self.login(p, &persona);
            }
            58..=64 => {
                let ms = *self.r.pick(&[0.0, 500.0, 1000.0, 5000.0, 30_000.0, 130_000.0]);
                self.step(json!({ "op": "advance", "ms": ms }));
            }
            65..=66 => {
                let vip = self.ps[p].vip;
                self.step(json!({ "op": "gone", "vip": vip }));
                self.ps[p].lobby = None;
                self.ps[p].buddy = None;
            }
            67..=68 => {
                let (vip, on) = (self.ps[p].vip, self.r.chance(0.5));
                self.step(json!({ "op": "slow", "vip": vip, "on": on }));
            }
            69..=71 => self.buddy(p),
            72..=73 => self.step(json!({ "op": "info" })),
            74..=75 => {
                let c = self.connect(p, PORT_REDIRECTOR);
                self.step(json!({ "op": "text", "conn": c, "kind": hs("@tic"), "text": hs("RC4") }));
                self.send(c, "@dir", vec![("PROD", s("nfs-pc-2005"))]);
                if self.r.chance(0.5) {
                    self.step(json!({ "op": "fin", "conn": c }));
                }
            }
            76..=77 => {
                let c = self.connect(p, PORT_HTTP);
                let req = *self.r.pick(&[
                    "GET /news HTTP/1.0\r\n\r\n",
                    "HEAD /TOS?x HTTP/1.1\r\n\r\n",
                    "GET /x HTTP/1.0\r\n\r\n",
                    "BAD\r\n\r\n",
                    "GET /news",
                ]);
                self.step(json!({ "op": "raw", "conn": c, "hex": hs(req) }));
                if self.r.chance(0.5) {
                    self.step(json!({ "op": "fin", "conn": c }));
                }
            }
            78..=83 => self.race(),
            84..=86 => {
                if self.r.chance(0.5) {
                    self.friends()
                } else {
                    self.kick()
                }
            }
            87..=91 => self.report(),
            92..=94 => {
                if let Some(l) = self.ps[p].lobby {
                    let junk =
                        self.r.pick(&["61016263000000000000000d00", "6d657367000000000000000c", "6d657367000000000000ffff00"]).to_string();
                    self.step(json!({ "op": "raw", "conn": l, "hex": junk }));
                }
            }
            95..=96 => {
                if let Some(b) = self.ps[p].buddy {
                    let op = if self.r.chance(0.5) { "fin" } else { "rst" };
                    self.step(json!({ "op": op, "conn": b }));
                }
            }
            _ => {
                if let Some(l) = self.ps[p].lobby {
                    self.step(json!({ "op": "fin", "conn": l }));
                    self.ps[p].lobby = None;
                }
            }
        }
    }
}

/// a session of `n` actions from `seed`
pub fn generate(seed: u64, n: usize) -> Session {
    generate_with(seed, n, false)
}

/// the same with odd switch frames and time passing on the timers' clock (`World::sleep`)
pub fn generate_ext(seed: u64, n: usize) -> Session {
    generate_with(seed, n, true)
}

fn generate_with(seed: u64, n: usize, ext: bool) -> Session {
    let vips = ["10.77.0.2", "10.77.0.3", "10.77.0.4", "10.77.0.5", "10.77.0.6", "10.77.0.7"];
    let groups = ["g2", "g3", "g4", "g5", "g6", "g6"];
    let ps = (0..6).map(|i| P { vip: vips[i], group: groups[i], name: NAMES[i], lobby: None, buddy: None }).collect();
    let mut g = Gen { r: Rng::new(seed), steps: Vec::new(), conns: 0, port: 40000, ps, races: Vec::new(), games: 0, ext };
    for (p, name) in NAMES.iter().enumerate() {
        g.login(p, name);
        g.buddy(p);
    }
    for _ in 0..n {
        g.action();
    }
    g.step(json!({ "op": "info" }));
    g.step(json!({ "op": "stop" }));
    Session { steps: g.steps }
}

fn latin1(v: &Value) -> String {
    crate::jsx::latin1_decode(&unhex(v.as_str().unwrap()))
}

/// What a session did: the frames the host sent, the lines it logged, the files its stop left (stats.json and the
/// friends lists' shelf), and the lobby's clock at the end
struct Run {
    frames: usize,
    lines: Vec<String>,
    stats: Option<String>,
    buddies: Vec<String>,
    t: f64,
}

/// the session on one online host, the lobby's counts checked after every step
fn run(s: &Session, dir: &std::path::Path) -> Run {
    let mut w = World::new(Opts { state_dir: Some(dir.to_path_buf()), ..Default::default() });
    w.t.set(T0);
    let mut conns: Vec<C> = Vec::new();
    for st in &s.steps {
        let conn = st.get("conn").and_then(Value::as_u64).map(|i| conns[i as usize]);
        let value = |w: &World, v: &Value| -> V {
            match v {
                Value::Number(n) => V::N(n.as_f64().unwrap()),
                Value::String(_) => V::S(latin1(v)),
                Value::Object(o) => {
                    let last = |i: &Value, kind: &str, k: &str| -> String {
                        w.c(conns[i.as_u64().unwrap() as usize]).last(kind).and_then(|m| m.get(k)).unwrap_or("").to_string()
                    };
                    if let Some(i) = o.get("lkey") {
                        V::S(last(i, "pers", "LKEY"))
                    } else if let Some(a) = o.get("ses") {
                        V::S(last(&a[0], "+ses", a[1].as_str().unwrap()))
                    } else if let Some(a) = o.get("resu") {
                        let n = crate::jsx::to_number(&last(&a[0], "+ses", "COUNT"));
                        let n = if w.c(conns[a[0].as_u64().unwrap() as usize]).last("+ses").is_some() { n as usize } else { 2 };
                        V::S(resu(n, a[1].as_u64().unwrap()))
                    } else {
                        panic!("a value: {v}")
                    }
                }
                _ => panic!("a value: {v}"),
            }
        };
        match st["op"].as_str().unwrap() {
            "connect" => {
                let vs = st["vip"].as_str().unwrap();
                let vip = ip(vs);
                if !w.clients.contains_key(&vip) {
                    w.client_of(vs, st["group"].as_str().unwrap());
                }
                let c = w.connect_as(
                    vip,
                    st["dport"].as_u64().unwrap() as u16,
                    st["port"].as_u64().unwrap() as u16,
                    st["cid"].as_u64().unwrap() as u32,
                );
                conns.push(c);
            }
            "send" => {
                let mut f: Fields = st["fields"].as_array().unwrap().iter().map(|p| (latin1(&p[0]).into(), value(&w, &p[1]))).collect();
                if let Some(i) = st.get("names").and_then(Value::as_u64)
                    && let Some(ses) = w.c(conns[i as usize]).last("+ses")
                {
                    let n = crate::jsx::to_number(ses.get("COUNT").unwrap_or("0")) as usize;
                    for k in 0..n {
                        f.push((format!("NAME{k}").into(), V::from(ses.get(&format!("OPPO{k}")))));
                    }
                }
                w.send(conn.unwrap(), &latin1(&st["kind"]), f);
            }
            "text" => w.send_text(conn.unwrap(), &latin1(&st["kind"]), &latin1(&st["text"])),
            "raw" => w.send_raw(conn.unwrap(), &unhex(st["hex"].as_str().unwrap())),
            "frame" => {
                let vs = st["vip"].as_str().unwrap();
                if !w.clients.contains_key(&ip(vs)) {
                    w.client_of(vs, st["group"].as_str().unwrap());
                }
                w.raw(ip(vs), &unhex(st["hex"].as_str().unwrap()));
            }
            "fin" => w.close(conn.unwrap()),
            "rst" => w.rst(conn.unwrap()),
            "advance" => w.advance(st["ms"].as_f64().unwrap()),
            "sleep" => w.sleep(st["ms"].as_f64().unwrap()),
            "gone" => w.peer_gone(ip(st["vip"].as_str().unwrap())),
            "slow" => {
                let vip = ip(st["vip"].as_str().unwrap());
                if st["on"].as_bool().unwrap() {
                    w.slow.lock().insert(vip);
                } else {
                    w.slow.lock().remove(&vip);
                }
            }
            "info" => {
                // (what /players and the console read of the host)
                let _ = (w.core.info(), w.core.vips());
            }
            "stop" => {
                w.core.stop();
                w.deliver();
            }
            op => panic!("a step: {op}"),
        }
        w.core.lobby.check_counts();
    }
    let read = |f: &str| std::fs::read_to_string(dir.join(f)).ok();
    let lines = w.lines.borrow().iter().map(|(l, _)| l.clone()).collect();
    let frames = w.sent.lock().len();
    Run { frames, lines, stats: read("stats.json"), buddies: crate::buddy::shelf_texts(dir), t: w.t.get() }
}

/// a session from its seed: consistent after every step, and its files read back as they were written
fn check(s: &Session, seed: u64) {
    let tmp = tempfile::tempdir().unwrap();
    let r = run(s, tmp.path());
    let clock = std::rc::Rc::new(std::cell::Cell::new(r.t));
    let quiet: crate::LogFn = std::rc::Rc::new(|_: &str, _: Option<&str>| {});
    let dir = Some(tmp.path().to_path_buf());
    if let Some(text) = &r.stats {
        let back = crate::stats::Stats::new(crate::stats::StatsOptions::new(dir.clone(), crate::stats::tests::clocks(clock), quiet));
        assert_eq!(*text, back.serialize(), "seed {seed}: stats.json read back");
    }
    assert_eq!(r.buddies, crate::buddy::shelf_reread(tmp.path()), "seed {seed}: the friends lists read back");
    // what the session went through (each seed's share of the rules)
    let n = |p: &str| r.lines.iter().filter(|l| l.contains(p)).count();
    eprintln!(
        "session {seed}: {} steps, {} frames, {} lines; results {}, not taken {}, differs {}, friends {}, asked {}, kicked {}, \
         ended {}, dropped {}, slow {}, refused {}, races {}",
        s.steps.len(),
        r.frames,
        r.lines.len(),
        n("ranked)") + n("ranked):"),
        n("not taken"),
        n("differs"),
        n("are friends"),
        n("to be friends"),
        n("kicked out"),
        n("ended:"),
        n("dropped:"),
        n("does not keep up"),
        n("refused"),
        n("the race starts"),
    );
}

#[test]
fn random_sessions_keep_the_lobby_consistent() {
    // (NFSU2_SESSIONS=<n>: n of them; 8 by default)
    let n: u64 = std::env::var("NFSU2_SESSIONS").ok().and_then(|v| v.parse().ok()).unwrap_or(8);
    for seed in 11..11 + n {
        check(&generate(seed, 900), seed);
    }
}

/// the same with odd switch frames, streams that reset in the middle of a message, overlapping sessions of one
/// browser, and time passing on the timers' clock (the services' time-outs, the game list's and the populations'
/// once-a-second, held-back presence, the Messenger login tried again, the files' saves)
#[test]
fn random_sessions_with_timers_keep_the_lobby_consistent() {
    // (NFSU2_SESSIONS=<n>: n of them; 8 by default)
    let n: u64 = std::env::var("NFSU2_SESSIONS").ok().and_then(|v| v.parse().ok()).unwrap_or(8);
    for seed in 1011..1011 + n {
        check(&generate_ext(seed, 900), seed);
    }
}
